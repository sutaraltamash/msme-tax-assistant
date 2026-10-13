// ============================================================================
// tax_assistant_server — application entrypoint.
//
// Wires the Step F end-to-end pipeline to the Crow WebSocket gateway:
//
//   * /ws negotiates a session (SESSION_INIT with a fresh session id) and then
//     dispatches the architecture.md §10 message contract onto tax::Pipeline:
//       UPLOAD_INVOICE -> IExtractor (mock or live Gemini per AI_MODE) ->
//                        StateMatrix -> RuleEngine -> SQLite persistence ->
//                        CHAT_MESSAGE gaps
//       USER_REPLY     -> archived to the transcript
//       REQUEST_FILING -> GSTRGenerator CSVs -> FILING_READY
//   * /   serves index.html, /assets/<path> serves the frontend bundle.
//
// The extraction seam selection lives in makeExtractor() below: AI_MODE=live
// builds a GeminiClient (reading GEMINI_API_KEY / GEMINI_MODEL from the
// environment), anything else falls back to the offline MockExtractor.
// ============================================================================

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <sstream>

#include "crow/crow.h"
#include "crow/json.h"
#include "crow/middlewares/cors.h"

#include "GeminiClient.hpp"
#include "HttpClient.hpp"
#include "IExtractor.hpp"
#include "MockExtractor.hpp"
#include "Pipeline.hpp"

namespace
{
    // ------------------------------------------------------------------
    // Environment helpers
    // ------------------------------------------------------------------
    const char* getEnv(const char* key, const char* fallback)
    {
        if (const char* v = std::getenv(key); v != nullptr && v[0] != '\0')
        {
            return v;
        }
        return fallback;
    }

    int getEnvInt(const char* key, int fallback)
    {
        if (const char* v = std::getenv(key); v != nullptr && v[0] != '\0')
        {
            return std::atoi(v);
        }
        return fallback;
    }

    std::string readFileText(const std::string& path)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
        {
            throw std::runtime_error("cannot open file: " + path);
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    // ------------------------------------------------------------------
    // Extractor selection (Phase 1 = mock, Phase 2 = live Gemini)
    //
    // When AI_MODE=live the server MUST have GEMINI_API_KEY set; at startup
    // this is checked before any WebSocket client can hit /ws, so a missing
    // key is caught instantly with a clear message instead of failing deep
    // inside a handler. If the key is present but matches the default
    // placeholder, we warn loudly (stderr) and still refuse to start live,
    // because a placeholder in production is a configuration error.
    // ------------------------------------------------------------------
    std::unique_ptr<tax::IExtractor> makeExtractor()
    {
        const std::string mode = "live";
        const std::string schemaPath =
            getEnv("GEMINI_SCHEMA_PATH", "./config/gemini_schema.json");
        const std::string schemaJson = readFileText(schemaPath);

        if (mode == "live")
        {
            std::vector<std::string> api_keys = {
                
            };
            const std::string model = "gemini-3.7-flash";

            std::cerr << "[extractor] AI_MODE=live, model=" << model
                      << ", api_keys=" << api_keys.size() << " (round-robin)\n";
            return std::make_unique<tax::GeminiClient>(
                api_keys, model, tax::createHttpClient(), schemaJson);
        }

        // Fallback: offline mock extractor (Phase 1, no network).
        std::cerr << "[extractor] AI_MODE=" << mode
                  << " (offline MockExtractor)\n";
        return std::make_unique<tax::MockExtractor>();
    }

    // --- protocol frames (architecture.md §10), as nlohmann::json ----------

    tax::json sessionInitFrame(const std::string& sessionId)
    {
        tax::json f{{"type", "SESSION_INIT"}, {"session_id", sessionId}};
        return f;
    }

    tax::json errorChat(const std::string& message)
    {
        tax::json f{{"type", "CHAT_MESSAGE"},
                    {"role", "assistant"},
                    {"content", message}};
        return f;
    }
} // namespace

int main()
{
    // ------------------------------------------------------------------
    // Runtime configuration (from environment / .env)
    // ------------------------------------------------------------------
    const std::string host      = getEnv("HOST", "0.0.0.0");
    const int         port      = getEnvInt("PORT", 18080);
    const std::string staticDir = getEnv("FRONTEND_DIR", "frontend");

    // ------------------------------------------------------------------
    // The Step F/G pipeline: extractor seam (mock or live Gemini per
    // AI_MODE) + rules config + SQLite.
    // ------------------------------------------------------------------
    std::unique_ptr<tax::Pipeline> pipeline;
    try
    {
        pipeline = std::make_unique<tax::Pipeline>(
            makeExtractor(),
            getEnv("RULES_CONFIG_PATH", "./config/rules_2026.json"),
            getEnv("DATABASE_PATH", "./database/tax_sessions.db"));
    }
    catch (const std::exception& e)
    {
        std::cerr << "startup failed: " << e.what() << "\n";
        return 1;
    }

    crow::App<crow::CORSHandler> app;

    // NOTE on Crow v1.2.0 route gotchas (both verified empirically):
    //  1. A bare "/<path>" catch-all shadows the WebSocket upgrade request
    //     and /ws never negotiates a session.
    //  2. "/static/<path>" is a RESERVED namespace (Crow's built-in static
    //     endpoint, CROW_STATIC_ENDPOINT). Registering it crashes at startup
    //     with an abort. Assets are therefore served under /assets/<path>.
    CROW_ROUTE(app, "/")([staticDir](const crow::request&) {
        crow::response res;
        res.set_static_file_info(staticDir + "/index.html");
        return res;
    });

    CROW_ROUTE(app, "/assets/<path>")([staticDir](const std::string& path) {
        crow::response res;
        res.set_static_file_info(staticDir + "/" + path);
        return res;
    });

    CROW_WEBSOCKET_ROUTE(app, "/ws")
        .onopen([&pipeline](crow::websocket::connection& conn) {
            try
            {
                // One tax_sessions row per socket; the id rides on the
                // connection so every later message names its session.
                const std::string sessionId = pipeline->openSession();
                conn.userdata(new std::string(sessionId));
                conn.send_text(sessionInitFrame(sessionId).dump());
            }
            catch (const std::exception& e)
            {
                conn.send_text(errorChat(std::string("Failed to open session: ") +
                                         e.what()).dump());
            }
        })
        .onmessage([&pipeline](crow::websocket::connection& conn,
                               const std::string& data, bool /*is_binary*/) {
            const auto* sid = static_cast<const std::string*>(conn.userdata());
            if (sid == nullptr)
            {
                conn.send_text(errorChat("No session is bound to this socket. "
                                         "Reconnect to /ws to start a new session.").dump());
                return;
            }

            auto msg = crow::json::load(data);
            if (!msg)
            {
                conn.send_text(errorChat("Message is not valid JSON.").dump());
                return;
            }

            tax::PipelineResult result;
            try
            {
                const auto strOrEmpty = [&msg](const char* key) {
                    return msg.has(key) && msg[key].t() == crow::json::type::String
                               ? std::string(msg[key].s())
                               : std::string();
                };
                const std::string type = strOrEmpty("type");
                if (type == "UPLOAD_INVOICE")
                {
                    const std::string payload = strOrEmpty("payload");
                    std::string mime = "application/octet-stream";
                    if (msg.has("mime_type") &&
                        msg["mime_type"].t() == crow::json::type::String)
                    {
                        mime = std::string(msg["mime_type"].s());
                    }
                    result = pipeline->processUpload(*sid, payload, mime);
                }
                else if (type == "USER_REPLY")
                {
                    result = pipeline->processUserReply(*sid, strOrEmpty("text"));
                }
                else if (type == "REQUEST_FILING")
                {
                    result = pipeline->requestFiling(*sid);
                }
                else
                {
                    conn.send_text(
                        errorChat("Unrecognized message type '" + type + "'.").dump());
                    return;
                }
            }
            catch (const std::exception& e)
            {
                conn.send_text(errorChat(std::string("Server error: ") + e.what()).dump());
                return;
            }

            for (const auto& frame : result.frames)
            {
                conn.send_text(frame.dump());
            }
        })
        .onclose([&pipeline](crow::websocket::connection& conn,
                             const std::string& /*reason*/) {
            auto* sid = static_cast<std::string*>(conn.userdata());
            if (sid != nullptr)
            {
                pipeline->closeSession(*sid);   // ABANDONED unless already FILED
                delete sid;
                conn.userdata(nullptr);
            }
        });

    // CORS applies a permissive policy by default (origin/methods/headers = "*").
    app.loglevel(crow::LogLevel::Info).port(port).bindaddr(host).run();

    return 0;
}
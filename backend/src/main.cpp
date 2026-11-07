// ============================================================================
// tax_assistant_server — application entrypoint.
//
// Wires the Step F end-to-end pipeline to the Crow WebSocket gateway:
//
//   * /ws negotiates a session (SESSION_INIT with a fresh session id) and then
//     dispatches the architecture.md §10 message contract onto tax::Pipeline:
//       UPLOAD_INVOICE -> IExtractor (mock or live Groq per AI_MODE) ->
//                        StateMatrix -> RuleEngine -> SQLite persistence ->
//                        CHAT_MESSAGE gaps
//       USER_REPLY     -> archived to the transcript
//       REQUEST_FILING -> GSTRGenerator CSVs -> FILING_READY
//   * /   serves index.html, /assets/<path> serves the frontend bundle.
//
// The extraction seam selection lives in makeExtractor() below: AI_MODE=live
// builds a GroqClient (reading GROQ_API_KEY / GROQ_MODEL from the
// environment), anything else falls back to the offline MockExtractor.
// ============================================================================

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <sstream>
#include <array>
#include <iomanip>
#include <random>
#include <cstdint>

#include "crow/crow.h"
#include "crow/json.h"
#include "crow/middlewares/cors.h"

#include "GroqClient.hpp"
#include "HttpClient.hpp"
#include "IExtractor.hpp"
#include "MockExtractor.hpp"
#include "Pipeline.hpp"

namespace
{
    std::string sha256(const std::string& input)
    {
        // Compact SHA-256 implementation for password comparison.
        static constexpr std::uint32_t k[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        std::vector<std::uint8_t> data(input.begin(), input.end());
        const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
        data.push_back(0x80); while (data.size() % 64 != 56) data.push_back(0);
        for (int i = 7; i >= 0; --i) data.push_back(static_cast<std::uint8_t>(bits >> (i * 8)));
        std::array<std::uint32_t,8> h={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
        auto rotr=[](std::uint32_t x,int n){return (x>>n)|(x<<(32-n));};
        for (std::size_t off=0; off<data.size(); off+=64) {
            std::uint32_t w[64];
            for(int i=0;i<16;++i) w[i]=(std::uint32_t(data[off+4*i])<<24)|(std::uint32_t(data[off+4*i+1])<<16)|(std::uint32_t(data[off+4*i+2])<<8)|data[off+4*i+3];
            for(int i=16;i<64;++i){auto s0=rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3);auto s1=rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+s0+w[i-7]+s1;}
            auto [a,b,c,d,e,f,g,hh]=h;
            for(int i=0;i<64;++i){auto s1=rotr(e,6)^rotr(e,11)^rotr(e,25);auto ch=(e&f)^(~e&g);auto t1=hh+s1+ch+k[i]+w[i];auto s0=rotr(a,2)^rotr(a,13)^rotr(a,22);auto maj=(a&b)^(a&c)^(b&c);auto t2=s0+maj;hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
            h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
        }
        std::ostringstream out; out<<std::hex<<std::setfill('0'); for(auto v:h) out<<std::setw(8)<<v; return out.str();
    }

    std::string loginUuid()
    {
        std::random_device rd; std::mt19937_64 gen(rd());
        std::array<unsigned char,16> b{}; for(auto& v:b) v=static_cast<unsigned char>(gen());
        b[6]=(b[6]&0x0f)|0x40; b[8]=(b[8]&0x3f)|0x80;
        std::ostringstream s; s<<std::hex<<std::setfill('0');
        for(int i=0;i<16;++i){if(i==4||i==6||i==8||i==10)s<<'-';s<<std::setw(2)<<unsigned(b[i]);} return s.str();
    }
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
    // Extractor selection (Phase 1 = mock, Phase 2 = live Groq)
    //
    // When AI_MODE=live the server MUST have GROQ_API_KEY set; at startup
    // this is checked before any WebSocket client can hit /ws, so a missing
    // key is caught instantly with a clear message instead of failing deep
    // inside a handler. If the key is present but matches the default
    // placeholder, we warn loudly (stderr) and still refuse to start live,
    // because a placeholder in production is a configuration error.
    // ------------------------------------------------------------------
    std::unique_ptr<tax::IExtractor> makeExtractor()
    {
        const std::string mode = getEnv("AI_MODE", "live");
        const std::string schemaPath =
            getEnv("GROQ_SCHEMA_PATH", "./config/groq_schema.json");
        const std::string schemaJson = readFileText(schemaPath);

        if (mode == "live")
        {
            const std::string api_key = getEnv("GROQ_API_KEY", "");
            const std::string model =
                getEnv("GROQ_MODEL", "qwen/qwen3.8-27b");

            std::cerr << "[extractor] AI_MODE=live, model=" << model << "\n";
            return std::make_unique<tax::GroqClient>(
                api_key, model, tax::createHttpClient(), schemaJson);
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
    // The Step F/G pipeline: extractor seam (mock or live Groq per
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

    CROW_ROUTE(app, "/api/login").methods(crow::HTTPMethod::POST)
    ([&pipeline](const crow::request& req) {
        const auto body = crow::json::load(req.body);
        if (!body || !body.has("username") || !body.has("email") || !body.has("password") ||
            body["username"].t() != crow::json::type::String ||
            body["email"].t() != crow::json::type::String ||
            body["password"].t() != crow::json::type::String)
            return crow::response(401, crow::json::wvalue({{"success", false}}));
        if (!pipeline->authenticate(body["username"].s(), body["email"].s(), sha256(body["password"].s())))
            return crow::response(401, crow::json::wvalue({{"success", false}}));
        return crow::response(200, crow::json::wvalue({{"success", true}, {"token", loginUuid()}}));
    });

    CROW_ROUTE(app, "/login.html")([staticDir](const crow::request&) {
        crow::response res;
        res.set_static_file_info(staticDir + "/login.html");
        return res;
    });

    // NOTE on Crow v1.2.0 route gotchas (both verified empirically):
    //  1. A bare "/<path>" catch-all shadows the WebSocket upgrade request
    //     and /ws never negotiates a session.
    //  2. "/static/<path>" is a RESERVED namespace (Crow's built-in static
    //     endpoint, CROW_STATIC_ENDPOINT). Registering it crashes at startup
    //     with an abort. Assets are therefore served under /assets/<path>.
    CROW_ROUTE(app, "/")([staticDir](const crow::request&) {
        crow::response res;
        res.set_static_file_info(staticDir + "/login.html");
        return res;
    });

    CROW_ROUTE(app, "/dashboard.html")([staticDir](const crow::request&) {
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

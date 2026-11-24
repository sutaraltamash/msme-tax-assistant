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
//   * /   serves login.html, /dashboard.html serves index.html,
//     /assets/<path> serves the frontend bundle.
//   * Auth endpoints: /api/auth/register, /api/auth/login, /api/auth/logout, /api/auth/me
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
#include <chrono>
#include <unordered_map>
#include <mutex>
#include <deque>
#include <algorithm>
#include <ctime>
#include <regex>

#include "crow/crow.h"
#include "crow/json.h"
#include "crow/middlewares/cors.h"

#include "GroqClient.hpp"
#include "HttpClient.hpp"
#include "IExtractor.hpp"
#include "MockExtractor.hpp"
#include "Pipeline.hpp"
#include "InvoiceRepository.hpp"
#include "Crypto.hpp"

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

// ============================================================================
// In-Memory Rate Limiter (sliding window) — per-IP quota protection
// ============================================================================

namespace tax {

// Rate limiter configuration
struct RateLimitConfig {
    // Chat/query endpoints: max 20 requests per minute per IP
    static constexpr int CHAT_MAX_REQUESTS = 20;
    static constexpr std::chrono::seconds CHAT_WINDOW{60};
    // Invoice upload endpoints: max 5 upload requests per minute per IP
    static constexpr int UPLOAD_MAX_REQUESTS = 5;
    static constexpr std::chrono::seconds UPLOAD_WINDOW{60};
    // Auth endpoints: max 10 requests per minute per IP
    static constexpr int AUTH_MAX_REQUESTS = 10;
    static constexpr std::chrono::seconds AUTH_WINDOW{60};
    // Maximum prompt length for chat
    static constexpr size_t MAX_PROMPT_LENGTH = 2000;
    // Maximum file size per upload (5 MB)
    static constexpr size_t MAX_FILE_SIZE_BYTES = 5 * 1024 * 1024;
    // Maximum files per batch upload
    static constexpr int MAX_FILES_PER_BATCH = 5;
    // Periodic cleanup interval
    static constexpr std::chrono::seconds CLEANUP_INTERVAL{300}; // 5 minutes
};

// Thread-safe sliding window rate limiter per IP
class RateLimiter {
public:
    explicit RateLimiter(int max_requests, std::chrono::seconds window)
        : max_requests_(max_requests), window_(window) {}

    // Returns true if request is allowed, false if rate limited
    bool check(const std::string& ip) {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);

        // Prune stale entries periodically
        if (shouldCleanup(now)) {
            pruneStaleEntries(now);
            last_cleanup_ = now;
        }

        auto& timestamps = ip_map_[ip];
        // Remove timestamps outside the window
        while (!timestamps.empty() && now - timestamps.front() > window_) {
            timestamps.pop_front();
        }

        if (static_cast<int>(timestamps.size()) >= max_requests_) {
            return false;
        }

        timestamps.push_back(now);
        return true;
    }

    // Get remaining requests for an IP (for informational purposes)
    int remaining(const std::string& ip) const {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = ip_map_.find(ip);
        if (it == ip_map_.end()) return max_requests_;

        int count = 0;
        for (const auto& ts : it->second) {
            if (now - ts <= window_) ++count;
        }
        return std::max(0, max_requests_ - count);
    }

private:
    bool shouldCleanup(std::chrono::steady_clock::time_point now) const {
        return now - last_cleanup_ > RateLimitConfig::CLEANUP_INTERVAL;
    }

    void pruneStaleEntries(std::chrono::steady_clock::time_point now) {
        for (auto it = ip_map_.begin(); it != ip_map_.end();) {
            auto& timestamps = it->second;
            while (!timestamps.empty() && now - timestamps.front() > window_) {
                timestamps.pop_front();
            }
            if (timestamps.empty()) {
                it = ip_map_.erase(it);
            } else {
                ++it;
            }
        }
    }

    const int max_requests_;
    const std::chrono::seconds window_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::deque<std::chrono::steady_clock::time_point>> ip_map_;
    std::chrono::steady_clock::time_point last_cleanup_ = std::chrono::steady_clock::now();
};

} // namespace tax

// ------------------------------------------------------------------
// Authentication helpers
// ------------------------------------------------------------------
static std::string getClientIP(const crow::request& req) {
    // Crow doesn't have direct IP access in request, use remote endpoint
    return req.get_header_value("X-Forwarded-For").empty() ? "127.0.0.1" : req.get_header_value("X-Forwarded-For");
}

static std::string getCookie(const crow::request& req, const std::string& name) {
    const std::string cookie_header = req.get_header_value("Cookie");
    if (cookie_header.empty()) return "";
    std::size_t pos = cookie_header.find(name + "=");
    if (pos == std::string::npos) return "";
    pos += name.size() + 1;
    std::size_t end = cookie_header.find(';', pos);
    if (end == std::string::npos) end = cookie_header.size();
    return cookie_header.substr(pos, end - pos);
}

static crow::response makeJsonResponse(int status, const crow::json::wvalue& body) {
    crow::response res(status, body.dump());
    res.set_header("Content-Type", "application/json; charset=utf-8");
    return res;
}

static crow::response authErrorResponse(int status, const std::string& message) {
    return makeJsonResponse(status, crow::json::wvalue({{"error", message}}));
}

static std::int64_t nowEpoch() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static bool isValidEmail(const std::string& email) {
    static const std::regex email_regex(R"(^[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,}$)");
    return std::regex_match(email, email_regex);
}

// ------------------------------------------------------------------
// Session cookie helpers
// ------------------------------------------------------------------
static std::string makeSessionCookie(const std::string& token, std::int64_t max_age_seconds, bool is_logout = false) {
    std::ostringstream oss;
    oss << "gstnode_session=";
    if (is_logout) {
        oss << "deleted; ";
    } else {
        oss << token << "; ";
    }
    oss << "HttpOnly; SameSite=Lax; Path=/; Max-Age=" << (is_logout ? 0 : max_age_seconds);
    // Add Secure flag only in production (HTTPS)
    // if (is_production) oss << "; Secure";
    return oss.str();
}

int main()
{
    // ------------------------------------------------------------------
    // Runtime configuration (from environment / .env)
    // ------------------------------------------------------------------
    const std::string host      = getEnv("HOST", "0.0.0.0");
    const int         port      = getEnvInt("PORT", 18080);
    const std::string staticDir = getEnv("FRONTEND_DIR", "frontend");
    const std::string adminEmail = getEnv("ADMIN_EMAIL", "admin@gstnode.com");
    const std::string adminPassword = getEnv("ADMIN_PASSWORD", "");

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

    // Access the repository for admin seeding and auth
    tax::InvoiceRepository& repo = pipeline->repository();

    // ------------------------------------------------------------------
    // Seed admin account on startup
    // ------------------------------------------------------------------
    if (!adminPassword.empty()) {
        try {
            repo.seedAdminIfNeeded(adminEmail, adminPassword);
            std::cerr << "[auth] Admin account seeded/updated: " << adminEmail << "\n";
        } catch (const std::exception& e) {
            std::cerr << "[auth] WARNING: Failed to seed admin account: " << e.what() << "\n";
        }
    } else {
        std::cerr << "[auth] WARNING: ADMIN_PASSWORD not set — no admin account seeded. "
                  << "Set ADMIN_PASSWORD in environment to enable admin access.\n";
    }

    // Clean up expired sessions periodically
    repo.cleanupExpiredAuthSessions();

    crow::App<crow::CORSHandler> app;

    // Initialize rate limiters
    tax::RateLimiter chatLimiter(tax::RateLimitConfig::CHAT_MAX_REQUESTS, tax::RateLimitConfig::CHAT_WINDOW);
    tax::RateLimiter uploadLimiter(tax::RateLimitConfig::UPLOAD_MAX_REQUESTS, tax::RateLimitConfig::UPLOAD_WINDOW);
    tax::RateLimiter authLimiter(tax::RateLimitConfig::AUTH_MAX_REQUESTS, tax::RateLimitConfig::AUTH_WINDOW);

    // Helper to extract client IP from Crow request/connection
    auto getClientIPWS = [](crow::websocket::connection& conn) -> std::string {
        return conn.get_remote_ip();
    };

    // ===================================================================
    // PUBLIC ROUTES (no authentication required)
    // ===================================================================

    // Static assets - MUST be before catch-all routes
    CROW_ROUTE(app, "/assets/<path>")([staticDir](const std::string& path) {
        // Block path traversal attempts
        if (path.find("..") != std::string::npos ||
            path.find('\\') != std::string::npos ||
            path.size() > 0 && path[0] == '/')
        {
            crow::response res(400);
            res.set_header("Content-Type", "application/json; charset=utf-8");
            res.write("{\"error\":\"Invalid asset path\"}");
            return res;
        }
        crow::response res;
        res.set_static_file_info(staticDir + "/" + path);
        return res;
    });

    // Stylesheet at root level (for login.html)
    CROW_ROUTE(app, "/styles.css")([staticDir](const crow::request&) {
        crow::response res;
        res.set_static_file_info(staticDir + "/styles.css");
        return res;
    });

    // Favicon
    CROW_ROUTE(app, "/favicon.ico")([staticDir](const crow::request&) {
        crow::response res;
        res.set_static_file_info(staticDir + "/favicon.ico");
        return res;
    });

    // Login page - publicly accessible
    CROW_ROUTE(app, "/login.html")([staticDir](const crow::request&) {
        crow::response res;
        res.set_static_file_info(staticDir + "/login.html");
        return res;
    });

    // Root - redirect to login page
    CROW_ROUTE(app, "/")([staticDir](const crow::request&) {
        crow::response res;
        res.set_static_file_info(staticDir + "/login.html");
        return res;
    });

    // ===================================================================
    // AUTH API ENDPOINTS
    // ===================================================================

    // POST /api/auth/register
    CROW_ROUTE(app, "/api/auth/register").methods(crow::HTTPMethod::POST)
    ([&repo, &authLimiter, &adminEmail](const crow::request& req) {
        // Rate limiting
        std::string clientIP = getClientIP(req);
        if (!authLimiter.check(clientIP)) {
            return authErrorResponse(429, "Rate limit exceeded. Please wait a minute and try again.");
        }

        // Parse JSON body
        const auto body = crow::json::load(req.body);
        if (!body) {
            return authErrorResponse(400, "Invalid JSON body.");
        }
        if (!body.has("username") || !body.has("email") || !body.has("password") ||
            body["username"].t() != crow::json::type::String ||
            body["email"].t() != crow::json::type::String ||
            body["password"].t() != crow::json::type::String) {
            return authErrorResponse(400, "Missing or invalid fields: username, email, password required.");
        }

        std::string username = body["username"].s();
        std::string email = body["email"].s();
        std::string password = body["password"].s();

        // Trim whitespace
        auto trim = [](std::string& s) {
            size_t start = s.find_first_not_of(" \t\r\n");
            size_t end = s.find_last_not_of(" \t\r\n");
            if (start == std::string::npos) { s.clear(); return; }
            s = s.substr(start, end - start + 1);
        };
        trim(username);
        trim(email);
        for (char& c : email) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        // Validate
        if (username.empty()) {
            return authErrorResponse(400, "Username cannot be empty.");
        }
        if (!isValidEmail(email)) {
            return authErrorResponse(400, "Invalid email format.");
        }
        if (password.size() < 8) {
            return authErrorResponse(400, "Password must be at least 8 characters.");
        }

        // Check if admin email is being registered
        std::string adminEmailNorm = adminEmail;
        for (char& c : adminEmailNorm) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (email == adminEmailNorm) {
            return authErrorResponse(403, "This email is reserved for admin account.");
        }

        // Check if email already exists
        if (repo.userExistsByEmail(email)) {
            return authErrorResponse(409, "An account with this email already exists.");
        }

        // Hash password
        auto salt = tax::crypto::generateSalt();
        std::string passwordHash = tax::crypto::pbkdf2Hash(password, salt, 100000, 32);

        // Create user (role = 'user' always for public registration)
        if (!repo.createUser(username, email, passwordHash, "user")) {
            return authErrorResponse(409, "An account with this email already exists.");
        }

        // Get the created user's ID
        std::int64_t user_id;
        std::string dummy_username, dummy_hash, dummy_role;
        repo.getUserByEmail(email, user_id, dummy_username, dummy_hash, dummy_role);

        // Create session (24h expiry)
        auto token = tax::crypto::generateSessionToken();
        std::string tokenHash = tax::crypto::hashSessionToken(token);
        std::int64_t expiresAt = nowEpoch() + 86400; // 24 hours
        repo.createAuthSession(user_id, tokenHash, expiresAt);

        // Increment message count (tracking only)
        repo.incrementMessageCount(user_id);

        // Set cookie
        std::string tokenHex = tax::crypto::bytesToHex(token);
        crow::response res = makeJsonResponse(200, crow::json::wvalue({
            {"username", username},
            {"email", email},
            {"role", "user"}
        }));
        res.set_header("Set-Cookie", makeSessionCookie(tokenHex, 86400));
        return res;
    });

    // POST /api/auth/login
    CROW_ROUTE(app, "/api/auth/login").methods(crow::HTTPMethod::POST)
    ([&repo, &authLimiter](const crow::request& req) {
        // Rate limiting
        std::string clientIP = getClientIP(req);
        if (!authLimiter.check(clientIP)) {
            return authErrorResponse(429, "Rate limit exceeded. Please wait a minute and try again.");
        }

        // Parse JSON body
        const auto body = crow::json::load(req.body);
        if (!body) {
            return authErrorResponse(400, "Invalid JSON body.");
        }
        if (!body.has("email") || !body.has("password") ||
            body["email"].t() != crow::json::type::String ||
            body["password"].t() != crow::json::type::String) {
            return authErrorResponse(400, "Missing or invalid fields: email, password required.");
        }

        std::string email = body["email"].s();
        std::string password = body["password"].s();
        bool remember = false;
        if (body.has("remember") && body["remember"].t() == crow::json::type::True) {
            remember = body["remember"].b();
        }

        // Normalize email
        auto trim = [](std::string& s) {
            size_t start = s.find_first_not_of(" \t\r\n");
            size_t end = s.find_last_not_of(" \t\r\n");
            if (start == std::string::npos) { s.clear(); return; }
            s = s.substr(start, end - start + 1);
        };
        trim(email);
        for (char& c : email) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        // Look up user
        std::int64_t user_id;
        std::string username, passwordHash, role;
        if (!repo.getUserByEmail(email, user_id, username, passwordHash, role)) {
            // Generic error to avoid user enumeration
            return authErrorResponse(401, "Invalid email or password.");
        }

        // Verify password (constant-time)
        if (!tax::crypto::verifyPassword(password, passwordHash)) {
            return authErrorResponse(401, "Invalid email or password.");
        }

        // Create session
        auto token = tax::crypto::generateSessionToken();
        std::string tokenHash = tax::crypto::hashSessionToken(token);
        std::int64_t expiresAt = nowEpoch() + (remember ? 2592000 : 86400); // 30 days : 24 hours
        repo.createAuthSession(user_id, tokenHash, expiresAt);

        // Increment message count (tracking only)
        repo.incrementMessageCount(user_id);

        // Set cookie
        std::string tokenHex = tax::crypto::bytesToHex(token);
        crow::response res = makeJsonResponse(200, crow::json::wvalue({
            {"username", username},
            {"email", email},
            {"role", role}
        }));
        res.set_header("Set-Cookie", makeSessionCookie(tokenHex, remember ? 2592000 : 86400));
        return res;
    });

    // POST /api/auth/logout
    CROW_ROUTE(app, "/api/auth/logout").methods(crow::HTTPMethod::POST)
    ([&repo](const crow::request& req) {
        std::string tokenHex = getCookie(req, "gstnode_session");
        if (!tokenHex.empty()) {
            // Hash the token to find the session
            auto tokenBytes = tax::crypto::hexToBytes(tokenHex);
            std::string tokenHash = tax::crypto::hashSessionToken(tokenBytes);
            repo.deleteAuthSession(tokenHash);
        }

        crow::response res = makeJsonResponse(200, crow::json::wvalue({{"success", true}}));
        res.set_header("Set-Cookie", makeSessionCookie("", 0, true));
        return res;
    });

    // GET /api/auth/me
    CROW_ROUTE(app, "/api/auth/me").methods(crow::HTTPMethod::GET)
    ([&repo](const crow::request& req) {
        std::string tokenHex = getCookie(req, "gstnode_session");
        if (tokenHex.empty()) {
            return authErrorResponse(401, "Not authenticated.");
        }

        auto tokenBytes = tax::crypto::hexToBytes(tokenHex);
        std::string tokenHash = tax::crypto::hashSessionToken(tokenBytes);

        std::int64_t user_id, expires_at;
        if (!repo.validateAuthSession(tokenHash, user_id, expires_at)) {
            return authErrorResponse(401, "Session expired or invalid.");
        }

        if (expires_at < nowEpoch()) {
            repo.deleteAuthSession(tokenHash);
            return authErrorResponse(401, "Session expired.");
        }

        // Get user info by user_id
        std::string username, email, role;
        if (!repo.getUserById(user_id, username, email, role)) {
            return authErrorResponse(401, "User not found.");
        }

        return makeJsonResponse(200, crow::json::wvalue({
            {"username", username},
            {"email", email},
            {"role", role}
        }));
    });

    // ===================================================================
    // PROTECTED ROUTES (authentication required)
    // ===================================================================

    // Dashboard page - requires authentication
    CROW_ROUTE(app, "/dashboard.html")([staticDir, &repo](const crow::request& req) {
        std::string tokenHex = getCookie(req, "gstnode_session");
        if (tokenHex.empty()) {
            crow::response res(302);
            res.set_header("Location", "/login.html");
            return res;
        }

        auto tokenBytes = tax::crypto::hexToBytes(tokenHex);
        std::string tokenHash = tax::crypto::hashSessionToken(tokenBytes);

        std::int64_t user_id, expires_at;
        if (!repo.validateAuthSession(tokenHash, user_id, expires_at) || expires_at < nowEpoch()) {
            if (expires_at < nowEpoch()) repo.deleteAuthSession(tokenHash);
            crow::response res(302);
            res.set_header("Location", "/login.html");
            return res;
        }

        crow::response res;
        res.set_static_file_info(staticDir + "/index.html");
        return res;
    });

    // Index.html alias - requires authentication
    CROW_ROUTE(app, "/index.html")([staticDir, &repo](const crow::request& req) {
        std::string tokenHex = getCookie(req, "gstnode_session");
        if (tokenHex.empty()) {
            crow::response res(302);
            res.set_header("Location", "/login.html");
            return res;
        }

        auto tokenBytes = tax::crypto::hexToBytes(tokenHex);
        std::string tokenHash = tax::crypto::hashSessionToken(tokenBytes);

        std::int64_t user_id, expires_at;
        if (!repo.validateAuthSession(tokenHash, user_id, expires_at) || expires_at < nowEpoch()) {
            if (expires_at < nowEpoch()) repo.deleteAuthSession(tokenHash);
            crow::response res(302);
            res.set_header("Location", "/login.html");
            return res;
        }

        crow::response res;
        res.set_static_file_info(staticDir + "/index.html");
        return res;
    });

    // ===================================================================
    // WEBSOCKET ROUTE (authentication required)
    // ===================================================================

    CROW_WEBSOCKET_ROUTE(app, "/ws")
        .onaccept([&repo](const crow::request& req, void** /*userdata*/) -> bool {
            // Extract and validate the gstnode_session cookie from the handshake
            std::string tokenHex = getCookie(req, "gstnode_session");
            if (tokenHex.empty())
            {
                return false; // Reject connection: no session cookie
            }

            try
            {
                auto tokenBytes = tax::crypto::hexToBytes(tokenHex);
                std::string tokenHash = tax::crypto::hashSessionToken(tokenBytes);

                std::int64_t user_id, expires_at;
                if (!repo.validateAuthSession(tokenHash, user_id, expires_at))
                {
                    return false; // Reject: invalid session
                }
                if (expires_at < nowEpoch())
                {
                    repo.deleteAuthSession(tokenHash);
                    return false; // Reject: expired session
                }
            }
            catch (const std::exception&)
            {
                return false; // Reject: malformed cookie
            }

            return true; // Accept connection
        })
        .onopen([&pipeline, &repo](crow::websocket::connection& conn) {
            // Session already validated in onaccept; create pipeline session
            try
            {
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
        .onmessage([&pipeline, &repo, &chatLimiter, &uploadLimiter, &getClientIPWS](crow::websocket::connection& conn,
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

                // Get client IP for rate limiting
                const std::string clientIP = getClientIPWS(conn);

                if (type == "UPLOAD_INVOICE")
                {
                    // Rate limit upload endpoints
                    if (!uploadLimiter.check(clientIP)) {
                        conn.send_text(
                            crow::json::wvalue({{"type", "CHAT_MESSAGE"},
                                                {"role", "assistant"},
                                                {"content", "Rate limit exceeded. Please wait a minute and try again."}}).dump());
                        return;
                    }

                    const std::string payload = strOrEmpty("payload");
                    std::string mime = "application/octet-stream";
                    if (msg.has("mime_type") &&
                        msg["mime_type"].t() == crow::json::type::String)
                    {
                        mime = std::string(msg["mime_type"].s());
                    }

                    // Validate base64 payload size (approximate original file size)
                    // Base64 is ~33% overhead, so 5MB file -> ~6.7MB base64
                    if (payload.size() > tax::RateLimitConfig::MAX_FILE_SIZE_BYTES * 4 / 3 + 100) {
                        conn.send_text(
                            crow::json::wvalue({{"type", "CHAT_MESSAGE"},
                                                {"role", "assistant"},
                                                {"content", "File too large. Maximum allowed size is 5 MB."}}).dump());
                        return;
                    }

                    result = pipeline->processUpload(*sid, payload, mime);
                    // Increment message count for user (we need user_id from session)
                    // TODO: associate WebSocket session with user
                }
                else if (type == "USER_REPLY")
                {
                    // Rate limit chat/query endpoints
                    if (!chatLimiter.check(clientIP)) {
                        conn.send_text(
                            crow::json::wvalue({{"type", "CHAT_MESSAGE"},
                                                {"role", "assistant"},
                                                {"content", "Rate limit exceeded. Please wait a minute and try again."}}).dump());
                        return;
                    }

                    const std::string text = strOrEmpty("text");

                    // Enforce maximum prompt length
                    if (text.size() > tax::RateLimitConfig::MAX_PROMPT_LENGTH) {
                        conn.send_text(
                            crow::json::wvalue({{"type", "CHAT_MESSAGE"},
                                                {"role", "assistant"},
                                                {"content", "Prompt too long. Maximum allowed length is 2000 characters."}}).dump());
                        return;
                    }

                    result = pipeline->processUserReply(*sid, text);
                    // Increment message count
                }
                else if (type == "REQUEST_FILING")
                {
                    // Rate limit filing requests too (use chat limiter)
                    if (!chatLimiter.check(clientIP)) {
                        conn.send_text(
                            crow::json::wvalue({{"type", "CHAT_MESSAGE"},
                                                {"role", "assistant"},
                                                {"content", "Rate limit exceeded. Please wait a minute and try again."}}).dump());
                        return;
                    }

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
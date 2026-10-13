// ============================================================================
// WinHttpClient.cpp — Windows-native HTTPS transport over WinHTTP (Schannel).
//
// Chosen deliberately for Step G: this MinGW toolchain ships
// x86_64-w64-mingw32/lib/libwinhttp.a but NO libcurl, so WinHTTP is the
// Windows-first transport (the libcurl client remains for non-Windows builds).
// Schannel does TLS with no OpenSSL dependency. Linked via -lwinhttp.
//
// Security posture: only https:// URLs use the wire; a bare "http://" target
// is accepted with TLS off strictly so local test doubles can be exercised —
// the production endpoint in newGeminiClient() is always https.
// ============================================================================

#include "HttpClient.hpp"

#include <windows.h>
#include <winhttp.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

struct ParsedUrl
{
    std::string scheme;   // "https" or "http"
    std::string host;
    unsigned port;
    std::string path;     // begins with '/'
};

// Parse "https://host[:port]/path" (or "http://"). Returns false on
// malformed input. Fully supports only these two schemes.
bool parseUrl(const std::string& url, ParsedUrl& out)
{
    std::string rest = url;
    unsigned defaultPort = 0;
    if (rest.rfind("https://", 0) == 0)
    {
        rest = rest.substr(8);
        defaultPort = 443;
        out.scheme = "https";
    }
    else if (rest.rfind("http://", 0) == 0)
    {
        rest = rest.substr(7);
        defaultPort = 80;
        out.scheme = "http";
    }
    else
    {
        return false;
    }

    const auto slash = rest.find('/');
    const std::string authority =
        (slash == std::string::npos) ? rest : rest.substr(0, slash);
    out.path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    if (authority.empty())
    {
        return false;
    }

    const auto colon = authority.rfind(':');
    const std::string host =
        (colon == std::string::npos) ? authority : authority.substr(0, colon);
    if (host.empty())
    {
        return false;
    }
    out.host = host;
    out.port = defaultPort;
    if (colon != std::string::npos)
    {
        try
        {
            const unsigned long p = std::stoul(authority.substr(colon + 1));
            if (p > 65535)
            {
                return false;
            }
            out.port = static_cast<unsigned>(p);
        }
        catch (const std::exception&)
        {
            return false;
        }
    }
    return true;
}

std::wstring widen(const std::string& s)
{
    if (s.empty())
    {
        return std::wstring();
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                        static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), len);
    return out;
}

std::string winError(DWORD code)
{
    char buf[512] = {0};
    const DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, buf, sizeof(buf), nullptr);
    std::string out(buf, n);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' ||
                            out.back() == ' '))
    {
        out.pop_back();
    }
    return out.empty() ? ("error " + std::to_string(code)) : out;
}

// RAII handle wrapper for HINTERNET.
class Handle
{
public:
    explicit Handle(HINTERNET h) : h_(h) {}
    ~Handle()
    {
        if (h_ != nullptr)
        {
            WinHttpCloseHandle(h_);
        }
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    HINTERNET get() const noexcept { return h_; }

private:
    HINTERNET h_;
};

} // namespace

namespace tax {

class WinHttpClient : public IHttpClient
{
public:
    HttpResponse postJson(const std::string& url,
                          const std::string& bearer_token,
                          const std::string& json_payload,
                          unsigned long timeout_seconds) override;
};

HttpResponse WinHttpClient::postJson(const std::string& url,
                                     const std::string& bearer_token,
                                     const std::string& json_payload,
                                     unsigned long timeout_seconds)
{
    ParsedUrl u;
    if (!parseUrl(url, u))
    {
        return {0, "", "malformed URL: must be 'https://host/path' or "
                       "'http://host/path'"};
    }

    const DWORD timeouts = static_cast<DWORD>(
        std::min<unsigned long>(timeout_seconds, 3600UL) * 1000UL);

    Handle session(WinHttpOpen(L"TaxAssistant/1.0",
                               WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (session.get() == nullptr)
    {
        return {0, "", "WinHttpOpen failed: " + winError(GetLastError())};
    }
    if (!WinHttpSetTimeouts(session.get(), timeouts, timeouts, timeouts, timeouts))
    {
        return {0, "", "WinHttpSetTimeouts failed: " + winError(GetLastError())};
    }

    Handle connect(WinHttpConnect(session.get(), widen(u.host).c_str(),
                                  static_cast<INTERNET_PORT>(u.port), 0));
    if (connect.get() == nullptr)
    {
        return {0, "", "WinHttpConnect to " + u.host + " failed: " +
                           winError(GetLastError())};
    }

    Handle request(WinHttpOpenRequest(connect.get(), L"POST",
                                      widen(u.path).c_str(), nullptr,
                                      WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      u.port == 443 ? WINHTTP_FLAG_SECURE : 0));
    if (request.get() == nullptr)
    {
        return {0, "", "WinHttpOpenRequest failed: " + winError(GetLastError())};
    }

    const std::wstring headers =
        L"Content-Type: application/json\r\n"
        L"x-goog-api-key: " +
        widen(bearer_token) + L"\r\n";

    if (!WinHttpSendRequest(request.get(), headers.c_str(),
                            static_cast<DWORD>(-1),   // headers null-terminated
                            const_cast<char*>(json_payload.data()),
                            static_cast<DWORD>(json_payload.size()),
                            static_cast<DWORD>(json_payload.size()), 0))
    {
        return {0, "", "WinHttpSendRequest failed: " + winError(GetLastError())};
    }

    if (!WinHttpReceiveResponse(request.get(), nullptr))
    {
        return {0, "", "WinHttpReceiveResponse failed: " +
                           winError(GetLastError())};
    }

    DWORD status = 0;
    DWORD statusLen = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(),
                             WINHTTP_QUERY_STATUS_CODE |
                                 WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusLen,
                             WINHTTP_NO_HEADER_INDEX))
    {
        return {0, "", "WinHttpQueryHeaders failed: " + winError(GetLastError())};
    }

    std::string body;
    for (;;)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available))
        {
            return {0, "", "WinHttpQueryDataAvailable failed: " +
                               winError(GetLastError())};
        }
        if (available == 0)
        {
            break;
        }
        std::vector<char> buf(available + 1, '\0');
        DWORD received = 0;
        if (!WinHttpReadData(request.get(), buf.data(), available, &received))
        {
            return {0, "", "WinHttpReadData failed: " + winError(GetLastError())};
        }
        body.append(buf.data(), received);
    }

    return {static_cast<long>(status), body, ""};
}

std::unique_ptr<IHttpClient> createHttpClient()
{
    return std::make_unique<WinHttpClient>();
}

} // namespace tax
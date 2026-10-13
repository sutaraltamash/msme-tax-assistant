// smoke_winhttp.cpp — minimal live smoke test for WinHTTP TLS + Gemini wire.
//
// Sends the smallest possible generateContent request (text-only, no image,
// temperature=0) and checks:
//   1. TLS handshake with generativelanguage.googleapis.com succeeds.
//   2. HTTP 200 is returned (not 401/403/5xx).
//   3. Response body is valid JSON with candidates[].
//
// Compile (one-shot, no CMake):
//   g++ -std=c++17 -I backend/include smoke_winhttp.cpp -lwinhttp -o smoke_winhttp.exe
// Run:
//   smoke_winhttp.exe <API_KEY> [MODEL]

#include <windows.h>
#include <winhttp.h>
#include <iostream>
#include <string>
#include <stdexcept>

#pragma comment(lib, "winhttp.lib")

// Tiny helper: wide ↔ narrow conversion (no ICU needed; ASCII-safe URLs/keys).
static std::wstring narrow_to_wide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}

static std::string do_post(const std::string& api_key, const std::string& model)
{
    // Minimal generateContent payload — text prompt only (no image inlining).
    const std::string body =
        "{\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":\"Say \\\"ok\\\"\"}]}],"
        "\"generationConfig\":{\"temperature\":0}}";

    const std::string path = "/v1beta/models/" + model + ":generateContent";

    HINTERNET session = WinHttpOpen(L"smoke_test/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) throw std::runtime_error("WinHttpOpen failed");

    HINTERNET conn = WinHttpConnect(session,
                                    L"generativelanguage.googleapis.com",
                                    INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) { WinHttpCloseHandle(session); throw std::runtime_error("WinHttpConnect failed"); }

    HINTERNET req = WinHttpOpenRequest(conn,
                                       L"POST",
                                       narrow_to_wide(path).c_str(),
                                       nullptr,
                                       WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       WINHTTP_FLAG_SECURE);
    if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(session); throw std::runtime_error("WinHttpOpenRequest failed"); }

    // Auth header: x-goog-api-key: <api_key>
    const std::wstring auth_hdr = L"x-goog-api-key: " + narrow_to_wide(api_key);
    WinHttpAddRequestHeaders(req, auth_hdr.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    WinHttpAddRequestHeaders(req, L"Content-Type: application/json", (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    BOOL sent = WinHttpSendRequest(req,
                                    WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                    (LPVOID)body.c_str(), (DWORD)body.size(),
                                    (DWORD)body.size(), 0);

    if (!sent || !WinHttpReceiveResponse(req, nullptr))
    {
        DWORD err = GetLastError();
        WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
        throw std::runtime_error("WinHttpSendRequest/ReceiveResponse error: " + std::to_string(err));
    }

    // Check HTTP status.
    DWORD status = 0;
    DWORD sz = sizeof(status);
    WinHttpQueryHeaders(req,
                         WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                         WINHTTP_HEADER_NAME_BY_INDEX,
                         &status, &sz, WINHTTP_NO_HEADER_INDEX);

    // Drain body.
    std::string response_body;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(req, &avail) && avail > 0)
    {
        std::string chunk(avail, '\0');
        DWORD read = 0;
        WinHttpReadData(req, chunk.data(), avail, &read);
        response_body.append(chunk, 0, read);
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);

    if (status != 200)
    {
        throw std::runtime_error("HTTP " + std::to_string(status) + ": " +
                                  response_body.substr(0, 512));
    }
    return response_body;
}

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cerr << "usage: smoke_winhttp.exe <API_KEY> [MODEL]\n";
        return 1;
    }
    const std::string api_key = argv[1];
    const std::string model   = argc >= 3 ? argv[2] : "gemini-3.7-flash";

    std::cout << "[smoke] target: generativelanguage.googleapis.com (HTTPS)\n";
    std::cout << "[smoke] model : " << model << "\n";
    std::cout << "[smoke] key   : <configured>\n";
    std::cout << "[smoke] sending minimal generateContent request...\n";

    try
    {
        const std::string body = do_post(api_key, model);
        std::cout << "[smoke] HTTP 200 OK — TLS handshake + wire succeeded.\n";

        // Quick structural check: must have "candidates".
        if (body.find("\"candidates\"") == std::string::npos)
        {
            std::cerr << "[smoke] WARN: 200 but no \"candidates\" in body:\n"
                      << body.substr(0, 512) << "\n";
            return 2;
        }

        // Print response snippet
        std::cout << "[smoke] response body: " << body.substr(0, 256) << "...\n";
        std::cout << "[smoke] PASS\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "[smoke] FAIL: " << e.what() << "\n";
        return 1;
    }
}

// ============================================================================
// HttpClient — synchronous HTTPS transport seam for the probabilistic layer.
//
// The decoupled-cloud-hybrid design (architecture.md §1) keeps the
// PROBABILISTIC layer (Gemini extractor) as the only network caller, and hides
// that one dependency behind this thin seam so Step G's GeminiClient can:
//
//   * be tested deterministically without a socket — a test injects a
//     canned-response FakeHttpClient;
//   * compile on every supported toolchain — the actual transport is chosen at
//     build time by CMake:
//       WIN32    -> WinHttpClient  (Windows native, Schannel TLS, -lwinhttp)
//       else     -> CurlHttpClient (libcurl, OpenSSL/whatever configure picked)
//
// Every method is synchronous and blocking; responses are returned as values.
// A transport failure is NOT an exception — it is reported via .status == 0
// and a human-readable .error (so the caller can decide how to surface it).
// ============================================================================

#pragma once

#include <memory>
#include <string>

namespace tax {

// A completed HTTP response. On a transport-level failure (DNS, TLS, refused,
// killed socket) `status` is 0 and `error` names the cause; `body` is then
// whatever (usually empty) bytes were received.
struct HttpResponse
{
    long status = 0;          // HTTP status code; 0 => transport failure
    std::string body;         // response body bytes (text)
    std::string error;        // empty on HTTP-level success
};

class IHttpClient
{
public:
    IHttpClient() = default;
    virtual ~IHttpClient() = default;
    IHttpClient(const IHttpClient&) = delete;
    IHttpClient& operator=(const IHttpClient&) = delete;

    // POST a JSON body with `Authorization: Bearer <bearer_token>`.
    //   url             — full "https://host[:port]/path" (https required for
    //                     production; the transport refuses plaintext unless
    //                     the scheme is explicitly "http://").
    //   bearer_token    — sent verbatim in the Authorization header.
    //   json_payload    — request body.
    //   timeout_seconds — total request timeout.
    virtual HttpResponse postJson(const std::string& url,
                                  const std::string& bearer_token,
                                  const std::string& json_payload,
                                  unsigned long timeout_seconds) = 0;
};

// Build the platform-default transport. WIN32 -> WinHttpClient (Schannel TLS),
// otherwise CurlHttpClient. Never returns nullptr.
std::unique_ptr<IHttpClient> createHttpClient();

} // namespace tax
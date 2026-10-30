// ============================================================================
// CurlHttpClient.cpp — libcurl HTTPS transport (non-Windows builds).
//
// Kept for the portable branch of the build matrix. Windows uses
// WinHttpClient.cpp instead (this MinGW toolchain ships no libcurl). Same
// IHttpClient seam, same semantics: synchronous POST, values out, transport
// failure reported as status==0 + error string, never an exception.
// ============================================================================

#include "HttpClient.hpp"

#include <curl/curl.h>

#include <cstring>
#include <memory>
#include <string>

namespace
{

std::size_t writeBody(char* ptr, std::size_t size, std::size_t nmemb, void* userdata)
{
    auto* sink = static_cast<std::string*>(userdata);
    sink->append(ptr, size * nmemb);
    return size * nmemb;
}

} // namespace

namespace tax {

class CurlHttpClient : public IHttpClient
{
public:
    HttpResponse postJson(const std::string& url,
                          const std::string& bearer_token,
                          const std::string& json_payload,
                          unsigned long timeout_seconds) override;
};

HttpResponse CurlHttpClient::postJson(const std::string& url,
                                      const std::string& bearer_token,
                                      const std::string& json_payload,
                                      unsigned long timeout_seconds)
{
    HttpResponse result;
    CURL* curl = curl_easy_init();
    if (curl == nullptr)
    {
        result.error = "curl_easy_init failed";
        return result;
    }
    const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> owned(curl,
                                                                    curl_easy_cleanup);

    curl_slist* head = nullptr;
    head = curl_slist_append(head, "Content-Type: application/json");
    const std::string auth = "Authorization: Bearer " + bearer_token;
    head = curl_slist_append(head, auth.c_str());
    if (head == nullptr)
    {
        result.error = "curl_slist_append failed";
        return result;
    }
    const std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> raw_headers(
        head, curl_slist_free_all);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, raw_headers.get());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(json_payload.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                     static_cast<long>(timeout_seconds));
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    const CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK)
    {
        result.status = 0;
        result.error = std::string("curl error: ") + curl_easy_strerror(rc);
        return result;
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    result.status = status;
    return result;
}

std::unique_ptr<IHttpClient> createHttpClient()
{
    return std::make_unique<CurlHttpClient>();
}

} // namespace tax
// ============================================================================
// GeminiClient — live invoice extraction behind the IExtractor seam (Step G).
//
// The real Phase-2 extractor: sends the uploaded invoice buffer to Google's
// generateContent endpoint (gemini-3.7-flash default, or whatever GEMINI_MODEL
// names) with a strict text prompt that embeds config/gemini_schema.json, and
// demands a single conforming JSON document back.
//
// Hard guarantees honored from the decoupled-cloud-hybrid contract:
//   * Probabilistic ONLY at extraction. No tax math happens here — StateMatrix
//     (deterministic layer) is the sole authority on amounts. This class ships
//     numeric facts through untouched and re-validated.
//   * Strict-schema enforcement at the C++ boundary. The returned document is
//     run through StateMatrix::validateAgainstSchema before it leaves this
//     class, so a hallucinated or malformed Gemini answer surfaces as
//     std::runtime_error at extraction time — never as partial data.
//   * Fail-fast construction. Missing api_key or a model name that cannot form
//     a URL is an exception at startup, not a silent mock fallback.
//   * Zero cloud persistence. The invoice bytes and the extracted document are
//     sent and immediately discarded; they are never stored server-side.
//
// The HTTP hop rides the IHttpClient seam so the whole behaviour is testable
// deterministically with canned responses (no network in the test suite).
// ============================================================================

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "HttpClient.hpp"
#include "IExtractor.hpp"
#include "json.hpp"

namespace tax {

using nlohmann::json;

class GeminiClient : public IExtractor
{
public:
    //   api_keys    — list of Google AI Studio API keys (for round-robin rotation).
    //   model       — model id, e.g. "gemini-3.7-flash".
    //   http        — transport seam (injectable for tests).
    //   schema_json — verbatim contents of config/gemini_schema.json; embedded
    //                 into the prompt and parsed for response validation.
    // Throws std::runtime_error on empty api_keys or an unparsable schema.
    GeminiClient(std::vector<std::string> api_keys, std::string model,
                 std::unique_ptr<IHttpClient> http, std::string schema_json,
                 unsigned long timeout_seconds = 60);

    // Convenience constructor for a single API key.
    GeminiClient(std::string api_key, std::string model,
                 std::unique_ptr<IHttpClient> http, std::string schema_json,
                 unsigned long timeout_seconds = 60);

    json extractFromBuffer(const std::string& base64_data,
                           const std::string& mime_type) const override;

    // Model id served by this client (for logging / diagnostics).
    const std::string& model() const noexcept { return model_; }

private:
    // Build the generateContent request payload (system_instruction carries the
    // schema contract; generationConfig pins responseMimeType=application/json
    // and temperature=0 for deterministic-ish extraction).
    json buildRequest(const std::string& base64_data,
                      const std::string& mime_type) const;

    // Parse a Gemini generateContent success body into the schema-conforming
    // document. Throws std::runtime_error on any structural failure.
    json parseSuccessResponse(const std::string& raw_body) const;

    std::vector<std::string> api_keys_;
    mutable std::atomic<std::size_t> key_index_{0};
    std::string model_;
    std::unique_ptr<IHttpClient> http_;
    json schema_;            // parsed config/gemini_schema.json (for validation)
    std::string schema_prompt_text_;
    unsigned long timeout_seconds_;
};

} // namespace tax
// ============================================================================
// GeminiClient.cpp — see header.
// ============================================================================

#include "GeminiClient.hpp"

#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "StateMatrix.hpp"

namespace tax {

namespace
{

// The model id is user-controlled (GEMINI_MODEL); URL-component it into a
// defensively-escaped path segment so it cannot smuggle query strings etc.
std::string urlComponent(const std::string& raw)
{
    std::string out;
    out.reserve(raw.size());
    for (const unsigned char c : raw)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                        c == '_';
        out += ok ? static_cast<char>(c) : '-';
    }
    return out;
}

// Strip a ```json ... ``` (or ``` ... ```) code fence the model may wrap the
// JSON in, then trim surrounding whitespace.
std::string stripCodeFence(const std::string& text)
{
    std::string t = text;
    auto trim = [](std::string& s) {
        std::size_t b = 0;
        while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' ||
                                s[b] == '\n'))
        {
            ++b;
        }
        std::size_t e = s.size();
        while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' ||
                         s[e - 1] == '\n'))
        {
            --e;
        }
        s = s.substr(b, e - b);
    };
    trim(t);
    if (t.size() >= 3 && t.compare(0, 3, "```") == 0)
    {
        const std::size_t e = t.find("```", 3);
        if (e != std::string::npos)
        {
            t = t.substr(3, e - 3);
            if (!t.empty() && t[0] == 'j' && t.compare(0, 4, "json") == 0)
            {
                t = t.substr(4);
            }
            trim(t);
        }
    }
    return t;
}

} // namespace

GeminiClient::GeminiClient(std::vector<std::string> api_keys, std::string model,
                           std::unique_ptr<IHttpClient> http,
                           std::string schema_json,
                           unsigned long timeout_seconds)
    : api_keys_(std::move(api_keys)),
      key_index_(0),
      model_(std::move(model)),
      http_(std::move(http)),
      timeout_seconds_(timeout_seconds)
{
    if (api_keys_.empty())
    {
        throw std::runtime_error("GeminiClient requires at least one GEMINI_API_KEY");
    }
    for (const auto& key : api_keys_)
    {
        if (key.empty())
        {
            throw std::runtime_error("GeminiClient requires non-empty API keys");
        }
    }
    if (model_.empty())
    {
        throw std::runtime_error("GeminiClient requires a non-empty GEMINI_MODEL");
    }
    if (!http_)
    {
        throw std::runtime_error("GeminiClient requires a non-null IHttpClient");
    }

    if (schema_json.empty())
    {
        throw std::runtime_error("GeminiClient requires the gemini schema JSON");
    }
    try
    {
        schema_ = json::parse(schema_json);
    }
    catch (const json::parse_error&)
    {
        throw std::runtime_error("GeminiClient schema JSON failed to parse");
    }

    schema_prompt_text_ =
        std::string(
            "You are a strict GST invoice-extraction engine. From the uploaded ")
        .append("file, return ONE valid JSON document that conforms EXACTLY to "
                "this schema:\n\n")
        .append(schema_json)
        .append(
            "\n\nRules:\n"
            "  * Return raw JSON only — no markdown, no commentary, no code "
            "fence.\n"
            "  * Every $required field must be present with its $type.\n"
            "  * money fields are INR, scale 2: use plain JSON numbers, do not "
            "quote them.\n"
            "  * supplier.address_state and buyer.address_state are two-letter "
            "state codes.\n"
            "  * invoice.date and irp.reported_on are YYYY-MM-DD strings.\n"
            "  * If a value is genuinely absent from the invoice, use '' or 0 "
            "or false as the schema allows; never invent data.\n");
}

GeminiClient::GeminiClient(std::string api_key, std::string model,
                           std::unique_ptr<IHttpClient> http,
                           std::string schema_json,
                           unsigned long timeout_seconds)
    : GeminiClient(std::vector<std::string>{std::move(api_key)},
                   std::move(model), std::move(http),
                   std::move(schema_json), timeout_seconds)
{
}

json GeminiClient::extractFromBuffer(const std::string& base64_data,
                                     const std::string& mime_type) const
{
    const std::string url =
        std::string("https://generativelanguage.googleapis.com/v1beta/models/") +
        urlComponent(model_) + ":generateContent";

    const std::string request_payload = buildRequest(base64_data, mime_type).dump();

    HttpResponse resp;
    constexpr int kMaxRetries = 3;
    for (int attempt = 0; attempt <= kMaxRetries; ++attempt)
    {
        // Round-robin selection of API key
        const std::size_t idx = key_index_.fetch_add(1, std::memory_order_relaxed);
        const std::string& current_key = api_keys_[idx % api_keys_.size()];

        resp = http_->postJson(url, current_key, request_payload, timeout_seconds_);

        if (resp.status == 503 && attempt < kMaxRetries)
        {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        break;
    }

    if (resp.status == 0)
    {
        throw std::runtime_error("Gemini request failed (transport): " +
                                 resp.error);
    }
    if (resp.status < 200 || resp.status >= 300)
    {
        // Surface the API's own error shape when present; never dump a secret.
        std::string brief;
        try
        {
            const json parsed = json::parse(resp.body);
            const auto& errs = parsed.value("error", json::object());
            brief = errs.value("message", "");
        }
        catch (const json::parse_error&)
        {
            brief = resp.body.substr(0, 256);
        }
        throw std::runtime_error("Gemini API error (HTTP " +
                                 std::to_string(resp.status) + "): " + brief);
    }

    json doc = parseSuccessResponse(resp.body);

    // Deterministic boundary check: refuse schema-violating output at the seam.
    std::vector<std::string> errors;
    if (!StateMatrix::validateAgainstSchema(doc, schema_, errors))
    {
        const std::string first =
            errors.empty() ? "unknown" : errors.front();
        throw std::runtime_error(
            "Gemini returned a document that did not pass the schema contract: " +
            first);
    }
    return doc;
}

json GeminiClient::buildRequest(const std::string& base64_data,
                                const std::string& mime_type) const
{
    return json{
        {"system_instruction", {{"parts", json::array({{{"text", schema_prompt_text_}}})}}},
        {"contents",
         json::array({json{
             {"role", "user"},
             {"parts",
              json::array({
                  {{"text",
                    "Extract the GST invoice from the uploaded file "
                    "now. Respond with a single JSON object conforming to the "
                    "schema in your instructions."}},
                  {{"inline_data", {{"mime_type", mime_type},
                                    {"data", base64_data}}}},
              })},
         }})},
        {"generationConfig",
         {{"responseMimeType", "application/json"}, {"temperature", 0}}},
    };
}

json GeminiClient::parseSuccessResponse(const std::string& raw_body) const
{
    json body;
    try
    {
        body = json::parse(raw_body);
    }
    catch (const json::parse_error& e)
    {
        throw std::runtime_error(std::string("Gemini response was not valid JSON: ") +
                                 e.what());
    }

    const auto hasCandidates = body.contains("candidates") &&
                               body["candidates"].is_array() &&
                               !body["candidates"].empty();
    if (!hasCandidates)
    {
        throw std::runtime_error(
            "Gemini response had no non-empty candidates[] (was extraction "
            "blocked by safety filters? check the response log)");
    }
    const auto& parts = body["candidates"][0]["content"]["parts"];
    if (!parts.is_array() || parts.empty() || !parts[0].is_object())
    {
        throw std::runtime_error(
            "Gemini response candidates[0].content.parts[0] was missing");
    }
    if (!parts[0].contains("text") || !parts[0]["text"].is_string())
    {
        throw std::runtime_error(
            "Gemini response part had no text field");
    }

    std::string text = stripCodeFence(parts[0]["text"].get<std::string>());
    try
    {
        json doc = json::parse(text);
        if (!doc.is_object())
        {
            throw std::runtime_error(
                "Gemini returned non-object JSON (expected an invoice document)");
        }
        return doc;
    }
    catch (const json::parse_error& e)
    {
        throw std::runtime_error(std::string("Gemini extraction text was not valid JSON: ") +
                                 e.what());
    }
}

} // namespace tax
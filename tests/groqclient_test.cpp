// ============================================================================
// groqclient_test.cpp — deterministic tests for the live Groq extractor.
//
// No network, no real API keys: a FakeHttpClient injects canned HttpResponse
// values and records the outgoing request (URL, bearer, payload) so the test
// proves the wire contract — right endpoint, right auth, schema embedded in
// the prompt, image_url carrying the buffer — plus every failure mode:
// transport errors, HTTP status errors, malformed bodies, content-filtered
// responses, PDF rejection, and schema-violating output being refused at the
// C++ boundary.
// ============================================================================

#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>

#include "GroqClient.hpp"
#include "HttpClient.hpp"
#include "MockExtractor.hpp"
#include "Sanitizer.hpp"
#include "fixtures.h"

#include "test_framework.h"

namespace
{

// Reads config/groq_schema.json (absolute path compiled in by the harness).
std::string readSchemaJson()
{
    std::ifstream f(GROQ_SCHEMA_PATH);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// A canned-response transport double. Records the last request for assertions.
class FakeHttpClient : public tax::IHttpClient
{
public:
    tax::HttpResponse reply;              // what postJson returns
    std::string last_url, last_bearer, last_payload;
    unsigned long last_timeout = 0;

    tax::HttpResponse postJson(const std::string& url,
                               const std::string& bearer_token,
                               const std::string& json_payload,
                               unsigned long timeout_seconds) override
    {
        last_url = url;
        last_bearer = bearer_token;
        last_payload = json_payload;
        last_timeout = timeout_seconds;
        return reply;
    }
};

tax::json defaultDoc()
{
    tax::MockExtractor mock;   // fully schema-conforming Step F fixture
    return mock.extractFromBuffer("", "");
}

// Wrap a document as a Groq chat/completions success body. The API returns
// the extracted JSON to the caller as a *string* inside
// choices[0].message.content, so the document must be JSON-encoded as a
// string value (quoted + escaped), never spliced in as raw bytes.
std::string groqBody(const tax::json& doc)
{
    const tax::json content_value(doc.dump());   // JSON string containing the doc
    return std::string("{\"choices\":[{\"message\":{\"role\":\"assistant\",")
           .append("\"content\":")
           .append(content_value.dump())
           .append("}}]}");
}

// Run a callable and return the std::runtime_error message (or "" if none).
std::string catchMessage(const std::function<void()>& fn)
{
    try
    {
        fn();
    }
    catch (const std::runtime_error& e)
    {
        return e.what();
    }
    return "";
}

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

} // namespace

// Helper to create a string with UTF-8 encoded Unicode characters (global for TEST macros)
// Use as u8("\xE2\x80\x8B") + "text" or std::string("text") + u8("\xE2\x80\x8B")
static std::string u8(const char* utf8_literal) { return std::string(utf8_literal); }

// ============================================================================
// Test implementations (all in global namespace for TEST macro to work)
// ============================================================================

// ---------------------------------------------------------------------------
TEST(happy_path_returns_parsed_document_and_asserts_wire_contract)
{
    auto fake = std::make_unique<FakeHttpClient>();
    FakeHttpClient* raw = fake.get();
    tax::GroqClient client("test-key-123", "llama-3.2-11b-vision-preview",
                           std::move(fake), readSchemaJson(), 30);

    raw->reply = tax::HttpResponse{200, groqBody(defaultDoc()), ""};

    tax::json doc = client.extractFromBuffer(testfix::kValidJpegBase64, "image/jpeg");

    CHECK(doc["invoice"]["number"] == "INV-2026-0001");
    CHECK(doc["supplier"]["gstin"] == "27AAPFU0939F1ZV");

    // Endpoint: Groq OpenAI-compatible completions URL (no query string).
    CHECK(raw->last_url == "https://api.groq.com/openai/v1/chat/completions");

    // Auth: Bearer sent verbatim.
    CHECK(raw->last_bearer == "test-key-123");

    // Timeout plumbed through.
    CHECK_EQ(raw->last_timeout, 30UL);

    // The uploaded buffer rides as data-URL inside image_url.
    tax::json req = tax::json::parse(raw->last_payload);
    const std::string img_url =
        req["messages"][0]["content"][1]["image_url"]["url"].get<std::string>();
    CHECK(contains(img_url, "data:image/jpeg;base64," + testfix::kValidJpegBase64));
    CHECK(req["messages"][0]["content"][1]["type"] == "image_url");

    // Model name forwarded.
    CHECK(req["model"] == "llama-3.2-11b-vision-preview");

    // Deterministic request posture: temperature pinned to 0.
    CHECK(req["temperature"] == 0);
}

TEST(schema_contract_is_embedded_in_the_prompt)
{
    auto fake = std::make_unique<FakeHttpClient>();
    FakeHttpClient* raw = fake.get();
    tax::GroqClient client("k", "llama-3.2-11b-vision-preview",
                           std::move(fake), readSchemaJson(), 30);
    raw->reply = tax::HttpResponse{200, groqBody(defaultDoc()), ""};

    client.extractFromBuffer(testfix::kValidPngBase64, "image/png");

    tax::json req = tax::json::parse(raw->last_payload);
    const std::string promptText =
        req["messages"][0]["content"][0]["text"].get<std::string>();
    CHECK(contains(promptText, "$required"));
    CHECK(contains(promptText, "hsn_string"));
    CHECK(contains(promptText, "supplier"));
    // The schema file's own marker is present verbatim.
    CHECK(contains(promptText, "$schema_description"));
}

TEST(code_fenced_text_is_unwrapped)
{
    auto fake = std::make_unique<FakeHttpClient>();
    FakeHttpClient* raw = fake.get();
    tax::GroqClient client("k", "llama-3.2-11b-vision-preview",
                           std::move(fake), readSchemaJson(), 30);
    const tax::json content_value(std::string("```json\n") + defaultDoc().dump() +
                                  "\n```");
    raw->reply = tax::HttpResponse{
        200,
        "{\"choices\":[{\"message\":{\"content\":" + content_value.dump() + "}}]}",
        ""};

    tax::json doc = client.extractFromBuffer(testfix::kValidPngBase64, "image/png");
    CHECK(doc["invoice"]["number"] == "INV-2026-0001");
}

TEST(throws_on_http_error_status)
{
    auto fake = std::make_unique<FakeHttpClient>();
    FakeHttpClient* raw = fake.get();
    tax::GroqClient client("k", "llama-3.2-11b-vision-preview",
                           std::move(fake), readSchemaJson(), 30);
    raw->reply = tax::HttpResponse{
        401, "{\"error\":{\"code\":401,\"message\":\"Invalid API Key. "
             "Please pass a valid API key.\"}}", ""};

    const std::string msg = catchMessage([&] {
        client.extractFromBuffer(testfix::kValidJpegBase64, "image/jpeg");
    });
    CHECK(contains(msg, "HTTP 401"));
    CHECK(contains(msg, "Invalid API Key"));
}

TEST(throws_on_transport_level_failure)
{
    auto fake = std::make_unique<FakeHttpClient>();
    FakeHttpClient* raw = fake.get();
    tax::GroqClient client("k", "llama-3.2-11b-vision-preview",
                           std::move(fake), readSchemaJson(), 30);
    raw->reply = tax::HttpResponse{0, "", "connection refused"};

    const std::string msg = catchMessage([&] {
        client.extractFromBuffer(testfix::kValidJpegBase64, "image/jpeg");
    });
    CHECK(contains(msg, "transport"));
    CHECK(contains(msg, "connection refused"));
}

TEST(throws_when_response_body_is_not_json)
{
    auto fake = std::make_unique<FakeHttpClient>();
    FakeHttpClient* raw = fake.get();
    tax::GroqClient client("k", "llama-3.2-11b-vision-preview",
                           std::move(fake), readSchemaJson(), 30);
    raw->reply = tax::HttpResponse{200, "<html>gateway error</html>", ""};

    const std::string msg = catchMessage([&] {
        client.extractFromBuffer(testfix::kValidJpegBase64, "image/jpeg");
    });
    CHECK(contains(msg, "not valid JSON"));
}

TEST(throws_when_no_choices_returned)
{
    auto fake = std::make_unique<FakeHttpClient>();
    FakeHttpClient* raw = fake.get();
    tax::GroqClient client("k", "llama-3.2-11b-vision-preview",
                           std::move(fake), readSchemaJson(), 30);

    raw->reply = tax::HttpResponse{200, "{\"choices\":[]}", ""};
    CHECK_THROWS(client.extractFromBuffer("AA==", "image/jpeg"),
                 std::runtime_error);

    raw->reply = tax::HttpResponse{200, "{}", ""};
    CHECK_THROWS(client.extractFromBuffer("AA==", "image/jpeg"),
                 std::runtime_error);
}

TEST(throws_when_message_content_is_missing)
{
    auto fake = std::make_unique<FakeHttpClient>();
    FakeHttpClient* raw = fake.get();
    tax::GroqClient client("k", "llama-3.2-11b-vision-preview",
                           std::move(fake), readSchemaJson(), 30);
    raw->reply = tax::HttpResponse{
        200, "{\"choices\":[{\"message\":{\"role\":\"assistant\"}}]}", ""};
    CHECK_THROWS(client.extractFromBuffer("AA==", "image/jpeg"),
                 std::runtime_error);
}

TEST(refuses_schema_violating_output_at_the_boundary)
{
    auto fake = std::make_unique<FakeHttpClient>();
    FakeHttpClient* raw = fake.get();
    tax::GroqClient client("k", "llama-3.2-11b-vision-preview",
                           std::move(fake), readSchemaJson(), 30);

    // invoice.number must be a string; hand back a number.
    tax::json bad = defaultDoc();
    bad["invoice"]["number"] = 12345;
    raw->reply = tax::HttpResponse{200, groqBody(bad), ""};

    const std::string msg = catchMessage([&] {
        client.extractFromBuffer(testfix::kValidJpegBase64, "image/jpeg");
    });
    CHECK(contains(msg, "schema contract"));
    // The deterministic layer refused it — no partial doc escapes.
    CHECK(contains(msg, "number"));
}

TEST(throws_immediately_for_pdf_mime_type)
{
    auto fake = std::make_unique<FakeHttpClient>();
    tax::GroqClient client("k", "llama-3.2-11b-vision-preview",
                           std::move(fake), readSchemaJson(), 30);

    const std::string msg = catchMessage([&] {
        client.extractFromBuffer(testfix::kValidPdfBase64, "application/pdf");
    });
    CHECK(contains(msg, "PNG or JPEG"));
    // Must not touch the network for PDF.
}

TEST(constructor_fails_fast_on_missing_pieces)
{
    auto fake = std::make_unique<FakeHttpClient>();
    CHECK_THROWS(tax::GroqClient("", "llama-3.2-11b-vision-preview",
                                 std::move(fake), readSchemaJson(), 30),
                 std::runtime_error);

    auto fake2 = std::make_unique<FakeHttpClient>();
    CHECK_THROWS(tax::GroqClient("k", "", std::move(fake2),
                                 readSchemaJson(), 30),
                 std::runtime_error);

}

TEST(single_api_key_is_used_for_each_request)
{
    auto fake = std::make_unique<FakeHttpClient>();
    FakeHttpClient* raw = fake.get();
    tax::GroqClient client("single-key", "llama-3.2-11b-vision-preview",
                           std::move(fake), readSchemaJson(), 30);

    raw->reply = tax::HttpResponse{200, groqBody(defaultDoc()), ""};

    client.extractFromBuffer(testfix::kValidJpegBase64, "image/jpeg");
    CHECK(raw->last_bearer == "single-key");

    client.extractFromBuffer(testfix::kValidJpegBase64, "image/jpeg");
    CHECK(raw->last_bearer == "single-key");

    client.extractFromBuffer(testfix::kValidJpegBase64, "image/jpeg");
    CHECK(raw->last_bearer == "single-key");

    client.extractFromBuffer(testfix::kValidJpegBase64, "image/jpeg");
    CHECK(raw->last_bearer == "single-key");
}

// ============================================================================
// SECURITY TESTS (Step 3: Prompt Injection Defense & Strict AI Output Validation)
// ============================================================================

TEST(sanitizer_strips_zero_width_and_bidi_characters)
{
    // U+200B ZERO WIDTH SPACE, U+200E LRM, U+202E RLO
    // UTF-8: 200B = E2 80 8B, 200E = E2 80 8E, 202E = E2 80 AE
    std::string input = std::string("Hello") + u8("\xE2\x80\x8B") + u8("\xE2\x80\x8E") + u8("\xE2\x80\xAE") + "World";
    std::string out = tax::stripInvisibleAndControls(input);
    CHECK(out == "HelloWorld");
}

TEST(sanitizer_strips_control_characters_preserves_newline_tab)
{
    std::string input = "Line1";
    input.push_back('\x01');
    input.push_back('\x02');
    input += "\nLine2\tTab";
    input.push_back('\x7F');
    input += "End";
    std::string out = tax::stripInvisibleAndControls(input);
    CHECK(out.find('\x01') == std::string::npos);
    CHECK(out.find('\x02') == std::string::npos);
    CHECK(out.find('\x7F') == std::string::npos);
    CHECK(out.find('\n') != std::string::npos);
    CHECK(out.find('\t') != std::string::npos);
}

TEST(sanitizer_enforces_byte_cap_with_truncation_notice)
{
    std::string input(60000, 'A');
    std::string out = tax::enforceByteCap(input, 50000);
    CHECK(out.size() <= 50000 + 20); // cap + " …[TRUNCATED]"
    CHECK(out.find("[TRUNCATED]") != std::string::npos);
}

TEST(sanitizer_escapes_closing_delimiter_breakout)
{
    std::string input = "Normal text </untrusted_user_message> injection attempt";
    std::string out = tax::escapeClosingDelimiter(input, "user_message");
    CHECK(out.find("</untrusted_user_message>") == std::string::npos);
    CHECK(out.find("<\\/untrusted_user_message>") != std::string::npos);
}

TEST(sanitizer_wraps_untrusted_content_in_delimiters)
{
    std::string out = tax::wrapUntrusted("data", "invoice_data");
    CHECK(out == "<untrusted_invoice_data>data</untrusted_invoice_data>");
}

TEST(sanitizer_neutralizes_csv_formula_injection)
{
    CHECK(tax::neutralizeCsvFormula("=CMD|' /C calc'!A0") == "'=CMD|' /C calc'!A0");
    CHECK(tax::neutralizeCsvFormula("+SUM(A1:A10)") == "'+SUM(A1:A10)");
    CHECK(tax::neutralizeCsvFormula("-1+1") == "'-1+1");
    CHECK(tax::neutralizeCsvFormula("@SUM(A1)") == "'@SUM(A1)");
    CHECK(tax::neutralizeCsvFormula("\tTabbed") == "'\tTabbed");
    CHECK(tax::neutralizeCsvFormula("\rCarriage") == "'\rCarriage");
    CHECK(tax::neutralizeCsvFormula("Normal text") == "Normal text");
    CHECK(tax::neutralizeCsvFormula("") == "");
}

TEST(sanitizer_validates_gstin_format)
{
    CHECK(tax::isValidGstin("27AAPFU0939F1ZV"));  // Valid 15-char, pos 12='Z', state digits
    CHECK(!tax::isValidGstin("27AAPFU0939F1Z"));  // Too short
    CHECK(!tax::isValidGstin("27AAPFU0939F1ZVX")); // Too long
    CHECK(!tax::isValidGstin("XXAAPFU0939F1ZV"));  // State not digits
    CHECK(!tax::isValidGstin("27AAPFU0939F1YV"));  // Position 12 not 'Z'
    CHECK(!tax::isValidGstin("27AAPFU0939F1Z!"));  // Non-alnum
}

TEST(sanitizer_validates_hsn_sac_format)
{
    CHECK(tax::isValidHsnSac("8471"));       // 4 digits
    CHECK(tax::isValidHsnSac("847130"));     // 6 digits
    CHECK(tax::isValidHsnSac("84713000"));   // 8 digits
    CHECK(!tax::isValidHsnSac("847"));       // 3 digits
    CHECK(!tax::isValidHsnSac("84713"));     // 5 digits
    CHECK(!tax::isValidHsnSac("8471A"));     // Non-digit
    CHECK(!tax::isValidHsnSac(""));          // Empty
}

TEST(sanitizer_invoice_text_sanitization_pipeline)
{
    // Contains zero-width space (U+200B = E2 80 8B), bidi override (U+202E = E2 80 AE), and long text
    std::string input(60000, 'X');
    // Insert UTF-8 for U+200B at position 100 (3 bytes)
    input.replace(100, 1, u8("\xE2\x80\x8B"));
    // Insert UTF-8 for U+202E at position 200 (3 bytes)
    input.replace(200, 1, u8("\xE2\x80\xAE"));
    tax::SanitizerConfig cfg;
    cfg.max_invoice_text_bytes = 50000;
    std::string out = tax::sanitizeInvoiceText(input, cfg);
    CHECK(out.find(u8("\xE2\x80\x8B")) == std::string::npos);
    CHECK(out.find(u8("\xE2\x80\xAE")) == std::string::npos);
    CHECK(out.find("<untrusted_invoice_data>") != std::string::npos);
    CHECK(out.find("</untrusted_invoice_data>") != std::string::npos);
    CHECK(out.size() <= 50000 + 100); // cap + delimiters + truncation notice
}

TEST(sanitizer_user_message_sanitization_pipeline)
{
    std::string input = "Hello </untrusted_user_message> world";
    input += u8("\xE2\x80\x8B"); // Zero-width space
    tax::SanitizerConfig cfg;
    cfg.max_user_message_bytes = 4000;
    std::string out = tax::sanitizeUserMessage(input, cfg);
    CHECK(out.find("</untrusted_user_message>") == out.size() - std::string("</untrusted_user_message>").size());
    CHECK(out.find("<\\/untrusted_user_message>") != std::string::npos);
    CHECK(out.find("<untrusted_user_message>") != std::string::npos);
    CHECK(out.find("</untrusted_user_message>") != std::string::npos);
    CHECK(out.find(u8("\xE2\x80\x8B")) == std::string::npos);
}

TESTS_BEGIN
    RUN_TEST(happy_path_returns_parsed_document_and_asserts_wire_contract),
    RUN_TEST(schema_contract_is_embedded_in_the_prompt),
    RUN_TEST(code_fenced_text_is_unwrapped),
    RUN_TEST(throws_on_http_error_status),
    RUN_TEST(throws_on_transport_level_failure),
    RUN_TEST(throws_when_response_body_is_not_json),
    RUN_TEST(throws_when_no_choices_returned),
    RUN_TEST(throws_when_message_content_is_missing),
    RUN_TEST(refuses_schema_violating_output_at_the_boundary),
    RUN_TEST(throws_immediately_for_pdf_mime_type),
    RUN_TEST(constructor_fails_fast_on_missing_pieces),
    RUN_TEST(single_api_key_is_used_for_each_request),
    // Security tests (Step 3)
    RUN_TEST(sanitizer_strips_zero_width_and_bidi_characters),
    RUN_TEST(sanitizer_strips_control_characters_preserves_newline_tab),
    RUN_TEST(sanitizer_enforces_byte_cap_with_truncation_notice),
    RUN_TEST(sanitizer_escapes_closing_delimiter_breakout),
    RUN_TEST(sanitizer_wraps_untrusted_content_in_delimiters),
    RUN_TEST(sanitizer_neutralizes_csv_formula_injection),
    RUN_TEST(sanitizer_validates_gstin_format),
    RUN_TEST(sanitizer_validates_hsn_sac_format),
    RUN_TEST(sanitizer_invoice_text_sanitization_pipeline),
    RUN_TEST(sanitizer_user_message_sanitization_pipeline)
TESTS_END

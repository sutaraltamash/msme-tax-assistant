// ============================================================================
// Sanitizer — lightweight text sanitization for prompt injection defense.
// Provides:
//   * Zero-width / bidirectional override character stripping
//   * Control character filtering (preserving \n, \r, \t)
//   * Context byte/character caps
//   * XML delimiter wrapping with closing-tag breakout escaping
//   * CSV formula injection neutralization
// No external dependencies; pure C++17 STL.
// ============================================================================

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace tax {

struct SanitizerConfig
{
    // Hard caps on text sent to / returned from Groq
    std::size_t max_invoice_text_bytes = 50000;    // ~50 KB extracted OCR text
    std::size_t max_user_message_bytes = 4000;     // user chat message cap
    std::size_t max_context_bytes      = 100000;   // total conversation context cap

    // CSV formula injection: prefix characters that trigger formula evaluation
    // Neutralized by prepending a single quote (Excel/Sheets convention)
    const char* csv_formula_prefixes = "=+-@\t\r";
};

/// Strip invisible/confusable Unicode and non-printable control characters.
/// Preserves: \n (0x0A), \r (0x0D), \t (0x09), and printable ASCII/UTF-8.
/// Removes: U+200B..U+200F (ZWSP, ZWNJ, ZWJ, LRM, RLM),
///          U+202A..U+202E (embedding overrides),
///          U+FEFF (BOM), and C0 controls except \n\r\t.
std::string stripInvisibleAndControls(std::string_view input);

/// Enforce a hard byte cap, truncating with a trailing notice if exceeded.
std::string enforceByteCap(std::string_view input, std::size_t max_bytes);

/// Escape occurrences of a closing XML delimiter inside untrusted text
/// so an attacker cannot break out of `<untrusted_...>...</untrusted_...>`.
/// Replaces `</tag>` with `<\/tag>` (backslash-escape the forward slash).
std::string escapeClosingDelimiter(std::string_view input, std::string_view tag_name);

/// Wrap untrusted content in explicit XML delimiters with breakout escaping.
/// Returns: `<untrusted_xxx>escaped_content</untrusted_xxx>`
std::string wrapUntrusted(std::string_view content, std::string_view tag_name);

/// Neutralize CSV formula injection: if field starts with = + - @ \t \r,
/// prepend a single quote so Excel/Sheets treats it as literal text.
std::string neutralizeCsvFormula(std::string_view field);

/// Validate Indian GSTIN: 15-character alphanumeric with structure:
/// 2 state code + 10 PAN + 1 entity + 1 'Z' + 1 checksum (we only check format here).
bool isValidGstin(std::string_view gstin);

/// Validate HSN/SAC code: digits-only, length 4, 6, or 8.
bool isValidHsnSac(std::string_view hsn);

/// Validate string field length against a max.
bool isValidLength(std::string_view s, std::size_t max_len);

/// Sanitization pipeline for invoice text extracted by Groq (OCR/raw text).
std::string sanitizeInvoiceText(std::string_view raw_text, const SanitizerConfig& cfg = {});

/// Sanitization pipeline for user chat messages sent to Groq.
std::string sanitizeUserMessage(std::string_view raw_text, const SanitizerConfig& cfg = {});

/// Sanitization pipeline for any string field from LLM output before
/// passing to RuleEngine / StateMatrix / SQLite / CSV.
std::string sanitizeLlmStringField(std::string_view raw, std::size_t max_len, const SanitizerConfig& cfg = {});

} // namespace tax
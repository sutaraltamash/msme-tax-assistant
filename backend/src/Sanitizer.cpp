// ============================================================================
// Sanitizer.cpp — implementation of prompt injection defenses.
// ============================================================================

#include "Sanitizer.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace tax {

namespace {

// Check if a Unicode codepoint (assumed valid UTF-8 decoded) is a
// zero-width / bidirectional override / BOM character we want to strip.
bool isInvisibleOrOverride(char32_t cp)
{
    // U+200B ZERO WIDTH SPACE
    // U+200C ZERO WIDTH NON-JOINER
    // U+200D ZERO WIDTH JOINER
    // U+200E LEFT-TO-RIGHT MARK (LRM)
    // U+200F RIGHT-TO-LEFT MARK (RLM)
    if (cp >= 0x200B && cp <= 0x200F) return true;

    // U+202A LEFT-TO-RIGHT EMBEDDING
    // U+202B RIGHT-TO-LEFT EMBEDDING
    // U+202C POP DIRECTIONAL FORMATTING
    // U+202D LEFT-TO-RIGHT OVERRIDE
    // U+202E RIGHT-TO-LEFT OVERRIDE
    if (cp >= 0x202A && cp <= 0x202E) return true;

    // U+FEFF ZERO WIDTH NO-BREAK SPACE (BOM)
    if (cp == 0xFEFF) return true;

    return false;
}

// Check if a codepoint is a C0 control character (0x00-0x1F) or DEL (0x7F)
// EXCEPT for \n (0x0A), \r (0x0D), \t (0x09) which we preserve.
bool isControlToStrip(char32_t cp)
{
    if (cp == 0x0A || cp == 0x0D || cp == 0x09) return false; // preserve \n \r \t
    if (cp <= 0x1F) return true;
    if (cp == 0x7F) return true;
    return false;
}

// Decode one UTF-8 codepoint from the string view, advancing the index.
// Returns the codepoint, or std::nullopt if invalid/incomplete sequence.
std::optional<char32_t> decodeUtf8(std::string_view s, std::size_t& i)
{
    if (i >= s.size()) return std::nullopt;

    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
        // ASCII
        ++i;  // advance index for single-byte codepoint
        return static_cast<char32_t>(c);
    }

    // Multi-byte sequence
    char32_t cp = 0;
    std::size_t remaining = 0;
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; remaining = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; remaining = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; remaining = 3; }
    else { return std::nullopt; } // invalid leading byte

    if (i + remaining >= s.size()) return std::nullopt;

    for (std::size_t k = 1; k <= remaining; ++k) {
        unsigned char cc = static_cast<unsigned char>(s[i + k]);
        if ((cc & 0xC0) != 0x80) return std::nullopt; // not a continuation byte
        cp = (cp << 6) | (cc & 0x3F);
    }

    i += remaining + 1;
    return cp;
}

} // anonymous namespace

std::string stripInvisibleAndControls(std::string_view input)
{
    std::string out;
    out.reserve(input.size());

    std::size_t i = 0;
    while (i < input.size()) {
        auto cp_opt = decodeUtf8(input, i);
        if (!cp_opt.has_value()) {
            // Invalid UTF-8: skip the byte (defensive)
            ++i;
            continue;
        }
        char32_t cp = cp_opt.value();

        if (isInvisibleOrOverride(cp) || isControlToStrip(cp)) {
            // Drop this character
            continue;
        }

        // Re-encode the codepoint to UTF-8
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    return out;
}

std::string enforceByteCap(std::string_view input, std::size_t max_bytes)
{
    if (input.size() <= max_bytes) {
        return std::string(input);
    }
    std::string out(input.substr(0, max_bytes));
    out += " …[TRUNCATED]";
    return out;
}

std::string escapeClosingDelimiter(std::string_view input, std::string_view tag_name)
{
    std::string full_tag = (tag_name.rfind("untrusted_", 0) == 0) ? std::string(tag_name) : ("untrusted_" + std::string(tag_name));
    std::string closing = "</" + full_tag + ">";
    std::string escaped_closing = "<\\/" + full_tag + ">";

    std::string out;
    out.reserve(input.size() + 16); // small reserve for potential escapes

    std::size_t pos = 0;
    while (pos < input.size()) {
        std::size_t found = input.find(closing, pos);
        if (found == std::string::npos) {
            out.append(input.substr(pos));
            break;
        }
        out.append(input.substr(pos, found - pos));
        out.append(escaped_closing);
        pos = found + closing.size();
    }
    return out;
}

std::string wrapUntrusted(std::string_view content, std::string_view tag_name)
{
    std::string out;
    out.reserve(content.size() + tag_name.size() * 2 + 10);
    out += "<untrusted_" + std::string(tag_name) + ">";
    out += content;
    out += "</untrusted_" + std::string(tag_name) + ">";
    return out;
}

std::string neutralizeCsvFormula(std::string_view field)
{
    if (field.empty()) return std::string(field);

    // Check first character (after any leading whitespace? No, CSV formulas
    // trigger on the very first character of the cell. But we also check
    // if the field starts with \t or \r which can appear at start.)
    char first = field[0];
    if (first == '=' || first == '+' || first == '-' || first == '@' ||
        first == '\t' || first == '\r') {
        return "'" + std::string(field);
    }
    return std::string(field);
}

bool isValidGstin(std::string_view gstin)
{
    if (gstin.size() != 15) return false;
    // Basic alphanumeric check (state code 2 digits, PAN 10 alnum, entity 1 digit, 'Z', checksum 1 alnum)
    // We don't validate the checksum algorithm here — just format.
    for (char c : gstin) {
        if (!std::isalnum(static_cast<unsigned char>(c))) return false;
    }
    // Check position 12 is 'Z' (0-indexed = 12)
    if (gstin[13] != 'Z') return false;
    // First two should be digits (state code)
    if (!std::isdigit(static_cast<unsigned char>(gstin[0])) ||
        !std::isdigit(static_cast<unsigned char>(gstin[1]))) return false;
    return true;
}

bool isValidHsnSac(std::string_view hsn)
{
    if (hsn.empty()) return false;
    std::size_t len = hsn.size();
    if (len != 4 && len != 6 && len != 8) return false;
    for (char c : hsn) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

bool isValidLength(std::string_view s, std::size_t max_len)
{
    return s.size() <= max_len;
}

std::string sanitizeInvoiceText(std::string_view raw_text, const SanitizerConfig& cfg)
{
    std::string s = stripInvisibleAndControls(raw_text);
    s = enforceByteCap(s, cfg.max_invoice_text_bytes);
    s = escapeClosingDelimiter(s, "untrusted_invoice_data");
    s = escapeClosingDelimiter(s, "invoice_data");
    s = wrapUntrusted(s, "invoice_data");
    return s;
}

std::string sanitizeUserMessage(std::string_view raw_text, const SanitizerConfig& cfg)
{
    std::string s = stripInvisibleAndControls(raw_text);
    s = enforceByteCap(s, cfg.max_user_message_bytes);
    s = escapeClosingDelimiter(s, "untrusted_user_message");
    s = escapeClosingDelimiter(s, "user_message");
    s = wrapUntrusted(s, "user_message");
    return s;
}

std::string sanitizeLlmStringField(std::string_view raw, std::size_t max_len, const SanitizerConfig& cfg)
{
    std::string s = stripInvisibleAndControls(raw);
    s = enforceByteCap(s, max_len);
    // Note: We don't wrap LLM output fields in delimiters — they're already
    // isolated by the C++ validation gate. The CSV neutralization happens
    // at export time in GSTRGenerator.
    return s;
}

} // namespace tax
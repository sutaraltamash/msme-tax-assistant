// ============================================================================
// StateMatrix.cpp — immutable tax ledger + deterministic integer-cent math.
// See StateMatrix.hpp for the contract. Rule evaluation and GSTR serialization
// consume this; nothing here talks to the network, database, or frontend.
// ============================================================================

#include "StateMatrix.hpp"
#include "Sanitizer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace tax {
namespace {

// ---------------------------------------------------------------------------
// Low-level primitives shared by validateAndSanitize and validateAgainstSchema
// ---------------------------------------------------------------------------

const std::string& requireString(const json& value, const std::string& path)
{
    if (!value.is_string())
    {
        throw std::invalid_argument("StateMatrix: " + path + ": expected a string");
    }
    return value.get_ref<const std::string&>();
}

// Scale the JSON rate (percent) to integer basis points (18.0 -> 1800).
int rateToBps(const json& value, const std::string& path)
{
    if (!value.is_number())
    {
        throw std::invalid_argument("StateMatrix: " + path + ": expected a number");
    }
    const double pct = value.get<double>();
    if (!std::isfinite(pct) || pct < 0.0)
    {
        throw std::invalid_argument("StateMatrix: " + path + ": rate must be finite and non-negative");
    }
    const double bps = pct * 100.0;
    if (bps > static_cast<double>(std::numeric_limits<int>::max()))
    {
        throw std::invalid_argument("StateMatrix: " + path + ": rate out of range");
    }
    return static_cast<int>(std::llround(bps));
}

// Valid Indian GST slabs in basis points (0%, 0.25%, 3%, 5%, 12%, 18%, 28%)
bool isValidGstRateBps(int rate_bps)
{
    static const int valid_rates[] = {0, 25, 300, 500, 1200, 1800, 2800};
    for (int r : valid_rates) {
        if (rate_bps == r) return true;
    }
    return false;
}

std::string trimAscii(std::string s)
{
    std::size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    std::size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

std::string normalizeGstin(std::string s)
{
    s = trimAscii(std::move(s));
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return s;
}

bool isPlaceholderValue(const std::string& raw)
{
    std::string s = trimAscii(raw);
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s.empty() || s == "n/a" || s == "na" || s == "none" || s == "null" ||
           s == "nil" || s == "-" || s.rfind("unregistered", 0) == 0;
}

std::string normalizeStateCode(std::string s)
{
    s = trimAscii(std::move(s));
    if (s.size() >= 2 && s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9') {
        if (s.size() == 2 || s[2] == ' ' || s[2] == '-' || s[2] == '(' || s[2] == ',' || s[2] == ':') {
            return s.substr(0, 2);
        }
    }
    return s;
}

std::string normalizeHsnString(std::string s, bool allow_multi_csv)
{
    s = trimAscii(std::move(s));
    if (allow_multi_csv && s.find(',') != std::string::npos) {
        std::string first_valid;
        std::size_t pos = 0;
        bool all_ok = true;
        while (pos <= s.size()) {
            std::size_t comma = s.find(',', pos);
            std::string part = trimAscii(comma == std::string::npos ? s.substr(pos) : s.substr(pos, comma - pos));
            std::string digits;
            for (char c : part) {
                if (c >= '0' && c <= '9') digits.push_back(c);
                else if (c != ' ' && c != '-') { all_ok = false; break; }
            }
            if (!all_ok || digits.empty() || digits.size() > 8) { all_ok = false; break; }
            if (first_valid.empty()) first_valid = digits;
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        if (all_ok && !first_valid.empty()) return first_valid;
        return s;
    }
    std::string compact;
    bool only_digits_sep = !s.empty();
    for (char c : s) {
        if (c >= '0' && c <= '9') compact.push_back(c);
        else if (c != ' ' && c != '-') { only_digits_sep = false; break; }
    }
    return only_digits_sep ? compact : s;
}

// Validate and sanitize a string field from LLM output with length cap
std::string validateStringField(const json& value, const std::string& path,
                                 std::size_t max_len, const char* field_name)
{
    if (!value.is_string())
    {
        throw std::invalid_argument("StateMatrix: " + path + ": expected a string");
    }
    std::string s = value.get<std::string>();
    // Sanitize invisible/control chars, then reject oversized strings
    s = stripInvisibleAndControls(s);
    if (s.size() > max_len) { throw std::invalid_argument("StateMatrix: " + path + ": " + field_name + " exceeds max length"); }
    return s;
}

// Validate GSTIN format (when non-empty)
void validateGstin(const std::string& gstin, const std::string& path)
{
    if (!gstin.empty() && !isValidGstin(gstin))
    {
        throw std::invalid_argument("StateMatrix: " + path + ": invalid GSTIN format (expected 15-char alphanumeric, pos 12='Z')");
    }
}

// Validate HSN/SAC format (when non-empty)
void validateHsnSac(const std::string& hsn, const std::string& path)
{
    if (hsn.empty()) return;
    if (hsn.size() > 8) { throw std::invalid_argument("StateMatrix: " + path + ": HSN/SAC exceeds 8 digits"); }
    for (char c : hsn) { if (c < '0' || c > '9') { throw std::invalid_argument("StateMatrix: " + path + ": HSN/SAC must be digits-only"); } }
}

} // anonymous namespace

// ===========================================================================
// money
// ===========================================================================

Cents money::toCents(const json& value)
{
    if (!value.is_number())
    {
        throw std::invalid_argument("StateMatrix: money value is not a number");
    }
    const double d = value.get<double>();
    if (!std::isfinite(d))
    {
        throw std::invalid_argument("StateMatrix: money value is not finite");
    }
    const double scaled = d * 100.0;
    if (scaled < 0.0 || scaled > 1.0e15)
    {
        throw std::invalid_argument("StateMatrix: money value out of range (negative or too large)");
    }
    return static_cast<Cents>(std::llround(scaled));
}

Cents money::taxOn(Cents base_cents, int rate_bps)
{
    if (base_cents < 0)
    {
        throw std::invalid_argument("StateMatrix: taxOn base must be non-negative");
    }
    if (rate_bps < 0)
    {
        throw std::invalid_argument("StateMatrix: taxOn rate must be non-negative");
    }
    // Guard the multiply: base_cents * rate_bps must fit in Cents plus the
    // +5000 half-up bias.
    if (base_cents != 0 &&
        rate_bps > (std::numeric_limits<Cents>::max() - 5000 - 1) / base_cents)
    {
        throw std::overflow_error("StateMatrix: taxOn overflow");
    }
    // Round-half-up to the nearest cent (paisa): /10000 because the divisor is
    // (rate_bps where 1% == 100 bps, over a cents base == rupees*100).
    return (base_cents * static_cast<Cents>(rate_bps) + 5000) / 10000;
}

// ===========================================================================
// Public API
// ===========================================================================

TaxBreakdown StateMatrix::computeTaxSplit(Cents base_cents, int tax_rate_bps,
                                          SupplyType type)
{
    TaxBreakdown b;
    switch (type)
    {
    case SupplyType::INTRA:
        // CGST and SGST each carry exactly half the combined statutory rate.
        // Per-head independent rounding mirrors the GSTN rule.
        b.cgst_cents        = money::taxOn(base_cents, tax_rate_bps / 2);
        b.sgst_cents        = money::taxOn(base_cents, tax_rate_bps / 2);
        b.has_cgst_sgst     = true;
        break;
    case SupplyType::INTER:
        b.igst_cents        = money::taxOn(base_cents, tax_rate_bps);
        b.has_igst          = true;
        break;
    }
    b.total_tax_cents = b.cgst_cents + b.sgst_cents + b.igst_cents + b.cess_cents;
    return b;
}

StateMatrix StateMatrix::validateAndSanitize(const json& doc)
{
    StateMatrix m;

    // fail(path, why) terminates the build with a precise diagnostic.
    const auto fail = [](const std::string& path, const std::string& why) {
        throw std::invalid_argument("StateMatrix::validateAndSanitize: " + path + ": " + why);
    };

    if (!doc.is_object())
    {
        fail("document", "root must be a JSON object");
    }

    // ---- supplier ----------------------------------------------------------
    if (!doc.contains("supplier") || !doc["supplier"].is_object())
    {
        fail("supplier", "required object not present");
    }
    const json& sup = doc["supplier"];
    m.supplier_gstin_  = normalizeGstin(validateStringField(sup.at("gstin"), "supplier.gstin", 32, "gstin"));
    m.supplier_name_   = validateStringField(sup.at("name"), "supplier.name", 200, "name");
    m.supplier_state_  = normalizeStateCode(validateStringField(sup.at("address_state"), "supplier.address_state", 50, "state"));
    if (m.supplier_state_.size() > 2) fail("supplier.address_state", "state exceeds max length");
    validateGstin(m.supplier_gstin_, "supplier.gstin");
    if (sup.contains("aato_crore") && !sup["aato_crore"].is_null())
    {
        if (!sup["aato_crore"].is_number())
        {
            fail("supplier.aato_crore", "must be a number");
        }
        const double aato = sup["aato_crore"].get<double>();
        if (aato < 0.0 || !std::isfinite(aato))
        {
            fail("supplier.aato_crore", "must be finite and non-negative");
        }
        m.aato_crore_ = aato;
    }

    // ---- buyer (optional) ---------------------------------------------------
    if (doc.contains("buyer") && !doc["buyer"].is_null())
    {
        const json& by = doc["buyer"];
        if (!by.is_object())
        {
            fail("buyer", "must be an object");
        }
        if (by.contains("registered") && !by["registered"].is_null())
        {
            if (!by["registered"].is_boolean())
            {
                fail("buyer.registered", "must be a boolean");
            }
            m.buyer_registered_ = by["registered"].get<bool>();
        }
        if (by.contains("gstin") && !by["gstin"].is_null())
        {
            std::string raw_gstin = validateStringField(by.at("gstin"), "buyer.gstin", 64, "gstin");
            if (!isPlaceholderValue(raw_gstin))
            {
                std::string gstin = normalizeGstin(std::move(raw_gstin));
                validateGstin(gstin, "buyer.gstin");
                m.buyer_gstin_ = std::move(gstin);
            }
        }
        if (by.contains("name") && !by["name"].is_null())
        {
            m.buyer_name_ = validateStringField(by.at("name"), "buyer.name", 200, "name");
        }
        if (by.contains("address_state") && !by["address_state"].is_null())
        {
            std::string st_code = normalizeStateCode(validateStringField(by.at("address_state"), "buyer.address_state", 50, "state"));
            if (!isPlaceholderValue(st_code))
            {
                if (st_code.size() > 2) fail("buyer.address_state", "state exceeds max length");
                m.buyer_state_ = std::move(st_code);
            }
        }
    }

    // ---- invoice -------------------------------------------------------------
    if (!doc.contains("invoice") || !doc["invoice"].is_object())
    {
        fail("invoice", "required object not present");
    }
    const json& inv = doc["invoice"];
    m.invoice_number_     = validateStringField(inv.at("number"), "invoice.number", 50, "number");
    m.invoice_date_       = trimAscii(validateStringField(inv.at("date"), "invoice.date", 32, "date"));
    if (m.invoice_date_.size() > 10 && (m.invoice_date_[10] == 'T' || m.invoice_date_[10] == ' ')) {
        m.invoice_date_ = m.invoice_date_.substr(0, 10);
    }
    if (m.invoice_date_.size() > 10) fail("invoice.date", "date exceeds max length");
    m.place_of_supply_    = normalizeStateCode(validateStringField(inv.at("place_of_supply"), "invoice.place_of_supply", 50, "place_of_supply"));
    m.hsn_string_         = normalizeHsnString(validateStringField(inv.at("hsn_string"), "invoice.hsn_string", 128, "hsn"), true);
    validateHsnSac(m.hsn_string_, "invoice.hsn_string");

    const std::string& st = requireString(inv.at("supply_type"), "invoice.supply_type");
    if (st == "INTRA")
    {
        m.supply_type_ = SupplyType::INTRA;
    }
    else if (st == "INTER")
    {
        m.supply_type_ = SupplyType::INTER;
    }
    else
    {
        fail("invoice.supply_type", "must be 'INTRA' or 'INTER'");
    }

    const std::string& cls = requireString(inv.at("classification"), "invoice.classification");
    if (cls == "B2B")
    {
        m.classification_ = Classification::B2B;
    }
    else if (cls == "B2C")
    {
        m.classification_ = Classification::B2C;
    }
    else if (cls == "UNDETERMINED")
    {
        m.classification_ = Classification::UNDETERMINED;
    }
    else
    {
        fail("invoice.classification", "must be 'B2B', 'B2C' or 'UNDETERMINED'");
    }

    if (!inv.contains("irn_present") || !inv["irn_present"].is_boolean())
    {
        fail("invoice.irn_present", "required boolean not present");
    }
    m.irn_present_ = inv["irn_present"].get<bool>();
    if (m.irn_present_)
    {
        if (!inv.contains("irn") || !inv["irn"].is_string())
        {
            fail("invoice.irn", "required string when irn_present is true");
        }
        m.irn_ = inv["irn"].get<std::string>();
    }

    if (!inv.contains("base_value"))
    {
        fail("invoice.base_value", "required number not present");
    }
    m.base_value_cents_ = money::toCents(inv["base_value"]);

    // ---- line items -----------------------------------------------------------
    if (!inv.contains("line_items") || !inv["line_items"].is_array())
    {
        fail("invoice.line_items", "required array not present");
    }
    const json& items = inv["line_items"];
    m.line_items_.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        const std::string liPath = "invoice.line_items[" + std::to_string(i) + "]";
        const json& li = items[i];
        if (!li.is_object())
        {
            fail(liPath, "line item must be an object");
        }
        LineItem item;
        item.hsn            = normalizeHsnString(validateStringField(li.at("hsn"), liPath + ".hsn", 32, "hsn"), false);
        validateHsnSac(item.hsn, liPath + ".hsn");
        item.tax_rate_bps   = rateToBps(li.at("tax_rate"), liPath + ".tax_rate");
        if (!isValidGstRateBps(item.tax_rate_bps))
        {
            fail(liPath + ".tax_rate", "invalid GST rate (valid slabs: 0, 0.25, 3, 5, 12, 18, 28)");
        }
        item.base_value_cents = money::toCents(li.at("base_value"));
        if (li.contains("description") && !li["description"].is_null())
        {
            item.description = validateStringField(li.at("description"), liPath + ".description", 500, "description");
        }
        if (li.contains("quantity") && !li["quantity"].is_null())
        {
            if (!li["quantity"].is_number())
            {
                fail(liPath + ".quantity", "must be a number");
            }
            double qty = li["quantity"].get<double>();
            if (!std::isfinite(qty) || qty < 0.0)
            {
                fail(liPath + ".quantity", "must be finite and non-negative");
            }
            item.quantity = qty;
        }
        if (li.contains("unit_price") && !li["unit_price"].is_null())
        {
            item.unit_price_cents = money::toCents(li["unit_price"]);
        }
        m.line_items_.push_back(std::move(item));
    }

    // ---- tax breakdown ----------------------------------------------------------
    if (!inv.contains("tax_breakdown") || !inv["tax_breakdown"].is_object())
    {
        fail("invoice.tax_breakdown", "required object not present");
    }
    const json& tb = inv["tax_breakdown"];
    if (tb.contains("cgst") && !tb["cgst"].is_null()) m.tax_.cgst_cents = money::toCents(tb["cgst"]);
    if (tb.contains("sgst") && !tb["sgst"].is_null()) m.tax_.sgst_cents = money::toCents(tb["sgst"]);
    if (tb.contains("igst") && !tb["igst"].is_null()) m.tax_.igst_cents = money::toCents(tb["igst"]);
    if (tb.contains("cess") && !tb["cess"].is_null()) m.tax_.cess_cents = money::toCents(tb["cess"]);
    if (tb.contains("total_tax") && !tb["total_tax"].is_null())
    {
        m.tax_.total_tax_cents = money::toCents(tb["total_tax"]);
    }
    else
    {
        m.tax_.total_tax_cents = m.tax_.cgst_cents + m.tax_.sgst_cents +
                                 m.tax_.igst_cents + m.tax_.cess_cents;
    }
    // Truth dwells in the amounts: the per-head flags are derived, not trusted
    // from the payload, so a contradiction in the booleans cannot propagate.
    m.tax_.has_cgst_sgst = (m.tax_.cgst_cents > 0 || m.tax_.sgst_cents > 0);
    m.tax_.has_igst      = (m.tax_.igst_cents > 0);

    if (!inv.contains("total_value"))
    {
        fail("invoice.total_value", "required number not present");
    }
    m.total_value_cents_ = money::toCents(inv["total_value"]);

    // ---- IRP metadata (optional) ------------------------------------------------
    if (doc.contains("irp") && !doc["irp"].is_null())
    {
        const json& irp = doc["irp"];
        if (!irp.is_object())
        {
            fail("irp", "must be an object");
        }
        if (irp.contains("reported_on") && !irp["reported_on"].is_null())
        {
            m.irp_reported_on_ = requireString(irp.at("reported_on"), "irp.reported_on");
        }
    }

    return m;
}

bool StateMatrix::validateAgainstSchema(const json& doc, const json& schema,
                                        std::vector<std::string>& out_errors)
{
    out_errors.clear();
    if (!schema.is_object())
    {
        out_errors.emplace_back("schema: root must be an object");
        return false;
    }
    if (!doc.is_object())
    {
        out_errors.emplace_back("document: root must be an object");
        return false;
    }

    // Fatal-vs-report: this walk never throws; it reports every discrepancy.
    struct Walker
    {
        static void node(const json& actual, const json& schema,
                         const std::string& path, std::vector<std::string>& errs)
        {
            // A schema node with no "$type" is a BARE object spec — e.g. the
            // groq_schema "line_items.$item" container. Default it to object
            // so per-field specs inside are walked, not rejected as unknown.
            std::string type = schema.value("$type", "");
            if (type.empty())
            {
                type = "object";
            }
            if (type == "object")
            {
                if (!actual.is_object())
                {
                    errs.push_back(path + ": expected object");
                    return;
                }
                for (auto it = schema.begin(); it != schema.end(); ++it)
                {
                    const std::string& key = it.key();
                    if (key.empty() || key.front() == '$') continue;   // metadata / $item
                    if (!it.value().contains("$type")) continue;       // description etc.
                    const bool required = it.value().value("$required", false);
                    if (!actual.contains(key))
                    {
                        if (required) errs.push_back(path + "." + key + ": missing required field");
                        continue;
                    }
                    node(actual[key], it.value(), path + "." + key, errs);
                }
            }
            else if (type == "array")
            {
                if (!actual.is_array())
                {
                    errs.push_back(path + ": expected array");
                    return;
                }
                if (schema.contains("$item"))
                {
                    const json& itemSchema = schema["$item"];
                    for (std::size_t i = 0; i < actual.size(); ++i)
                    {
                        node(actual[i], itemSchema,
                             path + "[" + std::to_string(i) + "]", errs);
                    }
                }
            }
            else if (type == "string")
            {
                if (!actual.is_string()) errs.push_back(path + ": expected string");
            }
            else if (type == "number")
            {
                if (!actual.is_number()) errs.push_back(path + ": expected number");
            }
            else if (type == "boolean")
            {
                if (!actual.is_boolean()) errs.push_back(path + ": expected boolean");
            }
            else
            {
                errs.push_back(path + ": unknown schema $type '" + type + "'");
            }
        }
    };

    for (auto it = schema.begin(); it != schema.end(); ++it)
    {
        const std::string& key = it.key();
        if (key.empty() || key.front() == '$') continue;
        if (!it.value().contains("$type")) continue;
        const bool required = it.value().value("$required", false);
        if (!doc.contains(key))
        {
            if (required) out_errors.push_back("document: missing required field '" + key + "'");
            continue;
        }
        Walker::node(doc[key], it.value(), key, out_errors);
    }

    return out_errors.empty();
}

} // namespace tax
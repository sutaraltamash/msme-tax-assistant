// ============================================================================
// GSTRGenerator.cpp — deterministic CSV serialization (see header).
// Pure functions: given validated StateMatrix documents, emit CSV text. No
// network, DB, file, or UI access; no floating-point money math.
// ============================================================================

#include "GSTRGenerator.hpp"
#include "Sanitizer.hpp"

#include <cstdio>
#include <map>
#include <utility>
#include <array>

namespace tax {

namespace {

// ---- formatting ------------------------------------------------------------

// Integer cents -> rupees string with exactly two decimals (100000 -> "1000.00").
std::string centsToRupees(Cents cents)
{
    const Cents whole = std::abs(static_cast<long long>(cents / 100));
    const Cents frac  = std::abs(static_cast<long long>(cents % 100));
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld.%02lld",
                  static_cast<long long>(whole), static_cast<long long>(frac));
    return buf;
}

// Basis points -> "18" (whole percent) or "1.25" (fractional).
std::string ratePercent(int rate_bps)
{
    if (rate_bps % 100 == 0)
    {
        return std::to_string(rate_bps / 100);
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", rate_bps / 100.0);
    return buf;
}

// Standard RFC-4180-esque escaping: quote if the field carries , " \n or \r.
// Also neutralizes CSV formula injection: if field starts with = + - @ \t \r,
// prepend a single quote so Excel/Sheets treats it as literal text.
std::string csvEscaped(const std::string& field)
{
    // First, neutralize CSV formula injection
    std::string safe_field = neutralizeCsvFormula(field);

    const bool needsQuote = safe_field.find_first_of(",\"\n\r") != std::string::npos;
    if (!needsQuote) return safe_field;
    std::string out = "\"";
    out.reserve(safe_field.size() + 2);
    for (const char ch : safe_field)
    {
        if (ch == '"') out.push_back('"');   // double up internal quotes
        out.push_back(ch);
    }
    out.push_back('"');
    return out;
}

std::string csvRow(const std::vector<std::string>& fields)
{
    std::string line;
    for (std::size_t i = 0; i < fields.size(); ++i)
    {
        if (i) line.push_back(',');
        line += csvEscaped(fields[i]);
    }
    line += "\r\n";
    return line;
}

// ---- GSTN-specific formatting helpers --------------------------------------

// Convert YYYY-MM-DD to DD-MMM-YYYY (e.g., 2026-11-12 -> 12-Nov-2026).
// If input isn't a valid YYYY-MM-DD string, pass it through unchanged.
std::string formatInvoiceDate(const std::string& dateStr)
{
    if (dateStr.size() != 10) return dateStr;
    if (dateStr[4] != '-' || dateStr[7] != '-') return dateStr;

    const std::string year = dateStr.substr(0, 4);
    const std::string month = dateStr.substr(5, 2);
    const std::string day = dateStr.substr(8, 2);

    // Validate numeric components
    int m, d, y;
    try {
        y = std::stoi(year);
        m = std::stoi(month);
        d = std::stoi(day);
    } catch (...) {
        return dateStr;
    }
    if (m < 1 || m > 12 || d < 1 || d > 31 || y < 1900 || y > 2100) {
        return dateStr;
    }

    static const std::array<const char*, 13> monthNames = {
        nullptr, "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };

    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d-%s-%04d", d, monthNames[m], y);
    return buf;
}

// Convert place_of_supply code (numeric "27" or alpha "MH") to GSTN "Code-StateName"
// format (e.g., "27" or "MH" -> "27-Maharashtra"). Covers all standard Indian
// GST state/UT codes 01-38 and 97. If unknown, pass through unchanged.
std::string formatPlaceOfSupply(const std::string& pos)
{
    if (pos.empty()) return pos;

    // Map of state code (numeric) -> state name
    static const std::map<std::string, std::string> stateNames = {
        {"01", "Jammu & Kashmir"},
        {"02", "Himachal Pradesh"},
        {"03", "Punjab"},
        {"04", "Chandigarh"},
        {"05", "Uttarakhand"},
        {"06", "Haryana"},
        {"07", "Delhi"},
        {"08", "Rajasthan"},
        {"09", "Uttar Pradesh"},
        {"10", "Bihar"},
        {"11", "Sikkim"},
        {"12", "Arunachal Pradesh"},
        {"13", "Nagaland"},
        {"14", "Manipur"},
        {"15", "Mizoram"},
        {"16", "Tripura"},
        {"17", "Meghalaya"},
        {"18", "Assam"},
        {"19", "West Bengal"},
        {"20", "Jharkhand"},
        {"21", "Odisha"},
        {"22", "Chhattisgarh"},
        {"23", "Madhya Pradesh"},
        {"24", "Gujarat"},
        {"25", "Daman & Diu"},
        {"26", "Dadra & Nagar Haveli"},
        {"27", "Maharashtra"},
        {"28", "Andhra Pradesh"},
        {"29", "Karnataka"},
        {"30", "Goa"},
        {"31", "Lakshadweep"},
        {"32", "Kerala"},
        {"33", "Tamil Nadu"},
        {"34", "Puducherry"},
        {"35", "Andaman & Nicobar Islands"},
        {"36", "Telangana"},
        {"37", "Andhra Pradesh (New)"},
        {"38", "Ladakh"},
        {"97", "Other Territory"}
    };

    // Map of state alpha code -> numeric code
    static const std::map<std::string, std::string> alphaToNumeric = {
        {"JK", "01"}, {"HP", "02"}, {"PB", "03"}, {"CH", "04"}, {"UT", "05"},
        {"HR", "06"}, {"DL", "07"}, {"RJ", "08"}, {"UP", "09"}, {"BR", "10"},
        {"SK", "11"}, {"AR", "12"}, {"NL", "13"}, {"MN", "14"}, {"MZ", "15"},
        {"TR", "16"}, {"ML", "17"}, {"AS", "18"}, {"WB", "19"}, {"JH", "20"},
        {"OD", "21"}, {"CG", "22"}, {"MP", "23"}, {"GJ", "24"}, {"DD", "25"},
        {"DN", "26"}, {"MH", "27"}, {"AP", "28"}, {"KA", "29"}, {"GA", "30"},
        {"LD", "31"}, {"KL", "32"}, {"TN", "33"}, {"PY", "34"}, {"AN", "35"},
        {"TS", "36"}, {"AP", "37"}, {"LA", "38"}, {"OT", "97"}
    };

    std::string numericCode = pos;

    // If it's already numeric (e.g., "27"), use it directly
    bool isNumeric = !pos.empty() && std::all_of(pos.begin(), pos.end(), ::isdigit);
    if (!isNumeric)
    {
        // Try alpha code lookup (case-insensitive)
        std::string upperPos = pos;
        std::transform(upperPos.begin(), upperPos.end(), upperPos.begin(), ::toupper);
        auto it = alphaToNumeric.find(upperPos);
        if (it != alphaToNumeric.end())
        {
            numericCode = it->second;
        }
        else
        {
            return pos; // unknown alpha code, pass through
        }
    }

    // Look up state name by numeric code
    auto it = stateNames.find(numericCode);
    if (it != stateNames.end())
    {
        return numericCode + "-" + it->second;
    }

    return pos; // unknown numeric code, pass through
}

// ---- per-leg aggregates ------------------------------------------------------

struct TaxAggregate
{
    Cents taxable = 0;
    Cents igst    = 0;
    Cents cgst    = 0;
    Cents sgst    = 0;
    Cents cess    = 0;
    double qty    = 0.0;

    Cents totalTax() const { return igst + cgst + sgst + cess; }
    void add(Cents base, const TaxBreakdown& split, double quantity)
    {
        taxable += base;
        igst    += split.igst_cents;
        cgst    += split.cgst_cents;
        sgst    += split.sgst_cents;
        cess    += split.cess_cents;
        qty     += quantity;
    }
};

// The statutory split for a line item, recomputed deterministically.
TaxBreakdown recomputeSplit(const StateMatrix& matrix, const LineItem& item)
{
    return StateMatrix::computeTaxSplit(item.base_value_cents,
                                        item.tax_rate_bps,
                                        matrix.supplyType());
}

std::string quantityString(double qty)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", qty);
    return buf;
}

} // anonymous namespace

// ===========================================================================
// GSTR-1 Table 4A — B2B outward supplies, one row per line item
// ===========================================================================

std::string GSTRGenerator::gstr1B2B(const std::vector<StateMatrix>& invoices)
{
    std::string out =
        "gstin_recipient,invoice_number,invoice_date,invoice_value,"
        "place_of_supply,supply_type,gst_rate,taxable_value,igst_amt,"
        "cgst_amt,sgst_amt,cess_amt\r\n";

    for (const auto& m : invoices)
    {
        if (m.classification() != Classification::B2B) continue;

        // Recompute the full statutory invoice value from the line items so
        // the printed total can never drift from deterministic math.
        Cents taxableTotal = 0;
        Cents taxTotal     = 0;
        for (const auto& li : m.lineItems())
        {
            taxableTotal += li.base_value_cents;
            taxTotal     += recomputeSplit(m, li).total_tax_cents;
        }
        const std::string invoiceValue = centsToRupees(taxableTotal + taxTotal);

        const std::string recipient = m.buyerGstin().value_or("");
        const std::string pois      = placeOfSupplyCode(m);
        const std::string type      = m.supplyType() == SupplyType::INTRA
                                          ? "Intra" : "Inter";

        for (const auto& li : m.lineItems())
        {
            const TaxBreakdown s = recomputeSplit(m, li);
            out += csvRow({recipient,
                           m.invoiceNumber(),
                           formatInvoiceDate(m.invoiceDate()),
                           invoiceValue,
                           formatPlaceOfSupply(pois),
                           type,
                           ratePercent(li.tax_rate_bps),
                           centsToRupees(li.base_value_cents),
                           centsToRupees(s.igst_cents),
                           centsToRupees(s.cgst_cents),
                           centsToRupees(s.sgst_cents),
                           centsToRupees(s.cess_cents)});
        }
    }
    return out;
}

// ===========================================================================
// GSTR-1 HSN summary — aggregated by (hsn, gst_rate)
// ===========================================================================

std::string GSTRGenerator::gstr1HsnSummary(const std::vector<StateMatrix>& invoices)
{
    std::string out =
        "hsn_code,gst_rate,taxable_value,igst_amt,cgst_amt,sgst_amt,"
        "cess_amt,quantity\r\n";

    std::map<std::pair<std::string, int>, TaxAggregate> agg;
    for (const auto& m : invoices)
    {
        for (const auto& li : m.lineItems())
        {
            const double qty = li.quantity.value_or(0.0);
            agg[{li.hsn, li.tax_rate_bps}].add(li.base_value_cents,
                                               recomputeSplit(m, li), qty);
        }
    }

    for (const auto& entry : agg)
    {
        const std::string& hsn  = entry.first.first;
        const int           bps = entry.first.second;
        const TaxAggregate& a   = entry.second;
        out += csvRow({hsn,
                       ratePercent(bps),
                       centsToRupees(a.taxable),
                       centsToRupees(a.igst),
                       centsToRupees(a.cgst),
                       centsToRupees(a.sgst),
                       centsToRupees(a.cess),
                       quantityString(a.qty)});
    }
    return out;
}

// ===========================================================================
// GSTR-3B Table 4 — outward supplies, one aggregate per INTRA/INTER bucket
// ===========================================================================

std::string GSTRGenerator::gstr3B(const std::vector<StateMatrix>& invoices)
{
    std::string out =
        "section,description,taxable_value,igst_amt,cgst_amt,sgst_amt,"
        "cess_amt,total_tax_amt\r\n";

    TaxAggregate intra, inter;
    bool hasIntra = false, hasInter = false;
    for (const auto& m : invoices)
    {
        TaxAggregate& bucket = m.supplyType() == SupplyType::INTRA
                                   ? intra : inter;
        bool&       present  = m.supplyType() == SupplyType::INTRA
                                   ? hasIntra : hasInter;
        for (const auto& li : m.lineItems())
        {
            if (!present) present = true;   // a bucket appears once it has any line
            bucket.add(li.base_value_cents, recomputeSplit(m, li),
                       li.quantity.value_or(0.0));
        }
    }

    const auto emit = [&](const char* section, const char* desc,
                          const TaxAggregate& a) {
        out += csvRow({section, desc,
                       centsToRupees(a.taxable),
                       centsToRupees(a.igst),
                       centsToRupees(a.cgst),
                       centsToRupees(a.sgst),
                       centsToRupees(a.cess),
                       centsToRupees(a.totalTax())});
    };
    if (hasIntra) emit("4A-Intra", "Outward taxable supplies - intra-state", intra);
    if (hasInter) emit("4A-Inter", "Outward taxable supplies - inter-state", inter);
    return out;
}

// ===========================================================================
// Place-of-supply resolution
// ===========================================================================

std::string GSTRGenerator::placeOfSupplyCode(const StateMatrix& matrix)
{
    const std::string& pos = matrix.placeOfSupply();
    if (pos == "same_as_supplier_state")
    {
        return matrix.supplierState();
    }
    return pos;
}

} // namespace tax
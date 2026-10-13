// ============================================================================
// GSTRGenerator.cpp — deterministic CSV serialization (see header).
// Pure functions: given validated StateMatrix documents, emit CSV text. No
// network, DB, file, or UI access; no floating-point money math.
// ============================================================================

#include "GSTRGenerator.hpp"

#include <cstdio>
#include <map>
#include <utility>

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
std::string csvEscaped(const std::string& field)
{
    const bool needsQuote = field.find_first_of(",\"\n\r") != std::string::npos;
    if (!needsQuote) return field;
    std::string out = "\"";
    out.reserve(field.size() + 2);
    for (const char ch : field)
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
                           m.invoiceDate(),
                           invoiceValue,
                           pois,
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
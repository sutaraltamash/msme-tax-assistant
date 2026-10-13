// ============================================================================
// GSTRGenerator — deterministic CSV serialization for GST returns.
//
// Reads validated StateMatrix documents and emits GST return CSV text. Part of
// the deterministic layer (architecture.md §1): the serialization performs NO
// floating-point arithmetic — every money value is integer cents formatted to
// fixed 2-decimal RUPEES, and statutory tax amounts are RECOMPUTED from the
// line items with StateMatrix::computeTaxSplit (half-up integer math). The
// payload's claimed tax_breakdown is never transcribed; a divergence between
// claimed totals and the recomputed figures is a compliance finding, not a
// transcription error, and is what the RuleEngine audit surfaces.
//
// Output contracts (documented so the CSVs are stable and testable):
//   * Header row is always emitted; an empty filing is header-only output.
//   * Row terminator is CRLF (GST offline utility convention on Windows).
//   * Fields containing , " \n or \r are quoted with doubled internal quotes.
//   * Money columns are RUPEES with exactly two decimals: 100000 cents -> "1000.00".
//   * Whole-figure gst_rate columns print as integers: 1800 bps -> "18".
//   * Rows are emitted deterministically: documents in input order; HSN
//     aggregates sorted by (hsn, gst_rate); GSTR-3B buckets in fixed order.
// ============================================================================

#pragma once

#include <string>
#include <vector>

#include "StateMatrix.hpp"

namespace tax {

class GSTRGenerator
{
public:
    // ---- GSTR-1, Table 4A (B2B outward supplies) --------------------------
    // One row per line item of every B2B-classified invoice (B2C/undetermined
    // invoices are excluded from this table). Column units: money in rupees,
    // gst_rate in percent. Columns:
    //   gstin_recipient, invoice_number, invoice_date, invoice_value,
    //   place_of_supply, supply_type, gst_rate, taxable_value, igst_amt,
    //   cgst_amt, sgst_amt, cess_amt
    //
    // invoice_value is the recomputed aggregate (sum of taxable lines + their
    // statutory tax), not the extraction's claimed total_value.
    static std::string gstr1B2B(const std::vector<StateMatrix>& invoices);

    // ---- GSTR-1, HSN summary ----------------------------------------------
    // Aggregates every line item of every invoice into one row per distinct
    // (hsn, gst_rate). quantity is the sum of per-line quantities where the
    // extraction provided them (0 otherwise). Columns:
    //   hsn_code, gst_rate, taxable_value, igst_amt, cgst_amt, sgst_amt,
    //   cess_amt, quantity
    static std::string gstr1HsnSummary(const std::vector<StateMatrix>& invoices);

    // ---- GSTR-3B, Table 4 (outward taxable supplies) ----------------------
    // One aggregated row per INTRA / INTER bucket that actually has line
    // items (an absent bucket is omitted). All supplies are treated as
    // regular (non-reverse-charge, taxable) outward supplies. Columns:
    //   section, description, taxable_value, igst_amt, cgst_amt, sgst_amt,
    //   cess_amt, total_tax_amt
    static std::string gstr3B(const std::vector<StateMatrix>& invoices);

private:
    // Resolve place_of_supply to a 2-letter state code for the return:
    // "same_as_supplier_state" expands to the supplier's state code.
    static std::string placeOfSupplyCode(const StateMatrix& matrix);
};

} // namespace tax
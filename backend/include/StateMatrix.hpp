// ============================================================================
// StateMatrix — immutable in-memory tax ledger + deterministic GST math core.
//
// The deterministic layer of the decoupled-cloud-hybrid design
// (architecture.md §1). Hard guarantees:
//
//   * 0% mathematical hallucination — every money value is stored as integer
//     CENTS and every tax computation is pure integer arithmetic. No float
//     participates in any calculation.
//   * Immutable by construction — validateAndSanitize() returns a fully-built
//     value and there is no mutation API; a new document yields a new matrix.
//     getSnapshot() hands back a defensive copy.
//   * Native validation — validateAndSanitize() enforces the
//     config/gemini_schema.json contract using nlohmann/json built-in type
//     checks (is_object/is_array/is_number/is_string/is_boolean). No external
//     JSON-Schema validator is used or required.
//
// The RuleEngine (Step D) reads facts from a matrix; it never mutates it.
// ============================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "json.hpp"

namespace tax {

using nlohmann::json;
using Cents = std::int64_t;

enum class SupplyType : std::uint8_t { INTRA, INTER };
enum class Classification : std::uint8_t { B2B, B2C, UNDETERMINED };

// ---------------------------------------------------------------------------
// money — deterministic integer-cent helpers
// ---------------------------------------------------------------------------
namespace money {

    // JSON number (rupees, 2-dp scale) -> integer cents. Rejects non-numbers,
    // non-finite values, negatives, and values that would overflow Cents.
    // Throws std::invalid_argument.
    Cents toCents(const json& value);

    // Statutory tax on an amount: roundHalfUp(base_cents * rate_bps / 10000).
    // Implements per-head rounding to the nearest paisa, matching the GSTN
    // computation rule. rate_bps is the rate in basis points (18% == 1800).
    // Throws std::invalid_argument on negative or overflowing inputs.
    Cents taxOn(Cents base_cents, int rate_bps);

} // namespace money

// ---------------------------------------------------------------------------
// TaxBreakdown — per-head split, always in integer cents.
// ---------------------------------------------------------------------------
struct TaxBreakdown
{
    Cents cgst_cents     = 0;
    Cents sgst_cents     = 0;
    Cents igst_cents     = 0;
    Cents cess_cents     = 0;
    Cents total_tax_cents = 0;
    bool  has_cgst_sgst  = false;
    bool  has_igst       = false;
};

inline bool operator==(const TaxBreakdown& a, const TaxBreakdown& b)
{
    return a.cgst_cents == b.cgst_cents && a.sgst_cents == b.sgst_cents &&
           a.igst_cents == b.igst_cents && a.cess_cents == b.cess_cents &&
           a.total_tax_cents == b.total_tax_cents &&
           a.has_cgst_sgst == b.has_cgst_sgst && a.has_igst == b.has_igst;
}

// ---------------------------------------------------------------------------
// LineItem — one extracted invoice line.
// ---------------------------------------------------------------------------
struct LineItem
{
    std::string hsn;
    std::string description;
    Cents base_value_cents = 0;   // should be non-negative
    int   tax_rate_bps     = 0;   // combined GST rate in basis points (18% == 1800)
    std::optional<double> quantity;
    std::optional<Cents>  unit_price_cents;
};

// ---------------------------------------------------------------------------
// StateMatrix
// ---------------------------------------------------------------------------
class StateMatrix
{
public:
    // ---- construction (fail-fast native validation) ----------------------
    // Validates and sanitizes an extracted invoice document against the
    // config/gemini_schema.json contract. Throws std::invalid_argument at the
    // first structural or type violation; never returns a partially-built
    // matrix.
    static StateMatrix validateAndSanitize(const json& doc);

    // ---- deterministic tax math (pure function, no state) -----------------
    // Computes the statutory CGST/SGST/IGST split for a line amount.
    //   INTRA -> CGST == SGST, each being the tax on half the combined rate,
    //            independently rounded (GSTN per-head paisa rounding).
    //   INTER -> the full combined rate as IGST.
    static TaxBreakdown computeTaxSplit(Cents base_cents, int tax_rate_bps,
                                        SupplyType type);

    // ---- generic schema type-walk (native nlohmann checks) ----------------
    // Walks `schema` (a gemini_schema.json-shaped tree) against `doc`.
    // Non-fatally appends human-readable errors; returns true only when the
    // document conforms exactly.
    static bool validateAgainstSchema(const json& doc, const json& schema,
                                      std::vector<std::string>& out_errors);

    // ---- snapshot API ------------------------------------------------------
    // Defensive copy. The caller may re-use the returned matrix freely — it
    // can never observe different values later because no mutation path
    // exists at all.
    [[nodiscard]] StateMatrix getSnapshot() const { return *this; }

    // ---- const accessors ---------------------------------------------------
    const std::string& supplierGstin() const noexcept { return supplier_gstin_; }
    const std::string& supplierName()  const noexcept { return supplier_name_; }
    const std::string& supplierState() const noexcept { return supplier_state_; }
    const std::optional<double>& aatoCrore() const noexcept { return aato_crore_; }

    bool hasBuyer() const noexcept { return buyer_gstin_.has_value() || buyer_registered_.has_value(); }
    const std::optional<std::string>& buyerGstin()     const noexcept { return buyer_gstin_; }
    const std::optional<std::string>& buyerName()      const noexcept { return buyer_name_; }
    const std::optional<std::string>& buyerState()     const noexcept { return buyer_state_; }
    const std::optional<bool>&        buyerRegistered() const noexcept { return buyer_registered_; }

    const std::string& invoiceNumber() const noexcept { return invoice_number_; }
    const std::string& invoiceDate()   const noexcept { return invoice_date_; }
    const std::string& placeOfSupply() const noexcept { return place_of_supply_; }
    SupplyType         supplyType()    const noexcept { return supply_type_; }
    Classification     classification() const noexcept { return classification_; }
    bool               irnPresent()    const noexcept { return irn_present_; }
    const std::string& irn()           const noexcept { return irn_; }
    const std::string& hsnString()     const noexcept { return hsn_string_; }
    const std::optional<std::string>& irpReportedOn() const noexcept { return irp_reported_on_; }

    const std::vector<LineItem>& lineItems() const noexcept { return line_items_; }
    Cents baseValueCents()  const noexcept { return base_value_cents_; }
    Cents totalTaxCents()   const noexcept { return tax_.total_tax_cents; }
    Cents totalValueCents() const noexcept { return total_value_cents_; }
    const TaxBreakdown& tax() const noexcept { return tax_; }

private:
    // ---- facts (set only by validateAndSanitize) ---------------------------
    std::string supplier_gstin_;
    std::string supplier_name_;
    std::string supplier_state_;
    std::optional<double> aato_crore_;

    std::optional<std::string> buyer_gstin_;
    std::optional<std::string> buyer_name_;
    std::optional<std::string> buyer_state_;
    std::optional<bool>        buyer_registered_;

    std::string invoice_number_;
    std::string invoice_date_;
    std::string place_of_supply_;
    std::string irn_;
    std::string hsn_string_;
    SupplyType       supply_type_    = SupplyType::INTRA;
    Classification   classification_ = Classification::UNDETERMINED;
    bool             irn_present_    = false;
    std::optional<std::string> irp_reported_on_;

    std::vector<LineItem> line_items_;
    Cents base_value_cents_  = 0;
    Cents total_value_cents_ = 0;
    TaxBreakdown tax_;
};

} // namespace tax
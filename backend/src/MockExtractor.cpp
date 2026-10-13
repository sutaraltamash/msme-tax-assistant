// ============================================================================
// MockExtractor.cpp — see header. Returns a controlled document so the Step F
// pipeline is testable with no network.
// ============================================================================

#include "MockExtractor.hpp"

namespace tax {

MockExtractor::MockExtractor() : document_(buildDefaultDocument()) {}

MockExtractor::MockExtractor(json document) : document_(std::move(document)) {}

json MockExtractor::extractFromBuffer(const std::string& /*base64_data*/,
                                      const std::string& /*mime_type*/) const
{
    return document_;   // deep copy each call — callers may mutate freely
}

// Fully compliant inter-state B2B enterprise invoice:
//   * supplier AATO 6.5 Cr > 5 Cr e-invoicing threshold (R-EINV-001 fires only
//     when IRN is absent — here IRN is present, so it passes)
//   * IRP block present (R-IRP-002 passes)
//   * POS "KA" != supplier state "MH" and IGST-only split (R-SPLIT-003/004 pass)
//   * hsn "84713000" is 8 digits (R-HSN-005 passes)
//   * B2B classification with a buyer GSTIN (R-CLASS-007 passes)
json MockExtractor::buildDefaultDocument()
{
    const std::string irn =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    return json{
        {"supplier", {{"gstin", "27AAPFU0939F1ZV"},
                      {"name", "Acme Retail Pvt Ltd"},
                      {"address_state", "MH"},
                      {"aato_crore", 6.5}}},
        {"buyer", {{"gstin", "29ABCDE1234F1Z5"},
                   {"name", "Karnataka Buyers"},
                   {"address_state", "KA"},
                   {"registered", true}}},
        {"invoice", {{"number", "INV-2026-0001"},
                     {"date", "2026-04-01"},
                     {"place_of_supply", "KA"},
                     {"supply_type", "INTER"},
                     {"classification", "B2B"},
                     {"irn_present", true},
                     {"irn", irn},
                     {"hsn_string", "84713000"},
                     {"base_value", 1000.00},
                     {"line_items",
                      json::array({{{"hsn", "84713000"},
                                    {"description", "Laptop"},
                                    {"quantity", 2},
                                    {"unit_price", 500.00},
                                    {"base_value", 1000.00},
                                    {"tax_rate", 18}}})},
                     {"tax_breakdown",
                      {{"cgst", 0},
                       {"sgst", 0},
                       {"igst", 180.00},
                       {"cess", 0},
                       {"has_cgst_sgst", false},
                       {"has_igst", true},
                       {"total_tax", 180.00}}},
                     {"total_value", 1180.00}}},
        {"irp", {{"reported_on", "2026-04-05"}, {"acknowledgment_no", "IRP-12345"}}},
        {"extraction_confidence", {{"overall_score", 0.98}, {"warnings", json::array()}}},
    };
}

} // namespace tax
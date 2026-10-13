// fixtures.h — shared invoice-document fixtures for the deterministic-core
// unit tests. Each builder mirrors the config/gemini_schema.json contract and
// is used verbatim by statematrix_test.cpp and ruleengine_test.cpp.
//
// The three base documents exercise the extraction-space grid:
//   * interB2BDoc — inter-state, B2B, IRN present with IRP metadata, AATO > threshold
//   * intraDoc    — intra-state, B2B, IRN present, NO irp block (an IRP gap)
//   * b2cDoc      — intra-state, B2C, no buyer details, no IRN, no AATO (rule gaps)
//
// A tax_breakdown helper is provided for mutations so tests can open the
// IGST / CGST+SGST legs without re-typing the whole structure.

#pragma once

#include "json.hpp"

namespace testfix {

using nlohmann::json;

// -- tax_breakdown builders -------------------------------------------------

inline json cgstSgst(int cgst, int sgst, int igst = 0, int total_tax = -1)
{
    json tb = json{
        {"cgst", cgst}, {"sgst", sgst}, {"igst", igst}, {"cess", 0},
        {"has_cgst_sgst", (cgst > 0 || sgst > 0)},
        {"has_igst", igst > 0},
    };
    tb["total_tax"] = (total_tax >= 0) ? total_tax : (cgst + sgst + igst);
    return tb;
}

inline json igstOnly(int igst, int total_tax = -1)
{
    json tb = json{
        {"cgst", 0}, {"sgst", 0}, {"igst", igst}, {"cess", 0},
        {"has_cgst_sgst", false}, {"has_igst", true},
    };
    tb["total_tax"] = (total_tax >= 0) ? total_tax : igst;
    return tb;
}

// -- base documents ----------------------------------------------------------

inline json interB2BDoc()
{
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
                     {"irn", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"},
                     {"hsn_string", "84713000"},
                     {"base_value", 1000.00},
                     {"line_items",
                      json::array({{{"hsn", "84713000"},
                                    {"description", "Laptop"},
                                    {"quantity", 2},
                                    {"unit_price", 500.00},
                                    {"base_value", 1000.00},
                                    {"tax_rate", 18}}})},
                     {"tax_breakdown", igstOnly(180.0)},
                     {"total_value", 1180.00}}},
        {"irp", {{"reported_on", "2026-04-05"}, {"acknowledgment_no", "IRP-12345"}}},
        {"extraction_confidence", {{"overall_score", 0.98}, {"warnings", json::array()}}},
    };
}

inline json intraDoc()
{
    return json{
        {"supplier", {{"gstin", "27AAPFU0939F1ZV"},
                      {"name", "Acme Retail Pvt Ltd"},
                      {"address_state", "MH"},
                      {"aato_crore", 6.5}}},
        {"buyer", {{"gstin", "27FGHIJ5678K1Z3"},
                   {"name", "Mumbai Buyers"},
                   {"address_state", "MH"},
                   {"registered", true}}},
        {"invoice", {{"number", "INV-2026-0002"},
                     {"date", "2026-04-02"},
                     {"place_of_supply", "same_as_supplier_state"},
                     {"supply_type", "INTRA"},
                     {"classification", "B2B"},
                     {"irn_present", true},
                     {"irn", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
                     {"hsn_string", "84713000"},
                     {"base_value", 1000.00},
                     {"line_items",
                      json::array({{{"hsn", "84713000"},
                                    {"description", "Laptop"},
                                    {"quantity", 1},
                                    {"unit_price", 1000.00},
                                    {"base_value", 1000.00},
                                    {"tax_rate", 18}}})},
                     {"tax_breakdown", cgstSgst(90.0, 90.0)},
                     {"total_value", 1180.00}}},
        {"extraction_confidence", {{"overall_score", 0.99}, {"warnings", json::array()}}},
    };
}

inline json b2cDoc()
{
    return json{
        {"supplier", {{"gstin", "27AAPFU0939F1ZV"},
                      {"name", "Acme Retail Pvt Ltd"},
                      {"address_state", "MH"}}},
        {"invoice", {{"number", "INV-2026-0003"},
                     {"date", "2026-04-03"},
                     {"place_of_supply", "same_as_supplier_state"},
                     {"supply_type", "INTRA"},
                     {"classification", "B2C"},
                     {"irn_present", false},
                     {"hsn_string", "84713000"},
                     {"base_value", 500.00},
                     {"line_items",
                      json::array({{{"hsn", "84713000"},
                                    {"description", "Accessory"},
                                    {"quantity", 1},
                                    {"base_value", 500.00},
                                    {"tax_rate", 18}}})},
                     {"tax_breakdown", cgstSgst(45.0, 45.0)},
                     {"total_value", 590.00}}},
    };
}

} // namespace testfix
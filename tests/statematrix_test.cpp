// statematrix_test.cpp — unit tests for the deterministic tax core.
//
// Covers: valid INTRA/INTER documents, tax-split math, per-head rounding edge
// cases, snapshot immutability, schema type-walking (inline + the real
// config/gemini_schema.json), and invalid JSON structures.

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "StateMatrix.hpp"
#include "fixtures.h"
#include "test_framework.h"

using namespace tax;
using namespace testfix;
using json = nlohmann::json;

namespace {

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(valid_intra_invoice)
{
    const auto m = StateMatrix::validateAndSanitize(intraDoc());

    CHECK_EQ(m.baseValueCents(), 100000LL);   // INR 1000.00
    CHECK_EQ(m.totalTaxCents(), 18000LL);     // INR 180.00
    CHECK_EQ(m.totalValueCents(), 118000LL);  // INR 1180.00
    CHECK(m.tax().has_cgst_sgst);
    CHECK(!m.tax().has_igst);
    CHECK_EQ(m.tax().cgst_cents, 9000LL);     // INR 90.00 each head
    CHECK_EQ(m.tax().sgst_cents, 9000LL);
    CHECK(m.supplyType() == SupplyType::INTRA);
    CHECK(m.classification() == Classification::B2B);
    CHECK(m.irnPresent());
    CHECK(m.irn() == "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    CHECK(m.hasBuyer());
    CHECK_EQ(m.lineItems().size(), 1u);
    CHECK_EQ(m.lineItems()[0].base_value_cents, 100000LL);
    CHECK_EQ(m.lineItems()[0].tax_rate_bps, 1800);
    CHECK(m.supplierState() == "MH");
}

TEST(valid_inter_invoice)
{
    const auto m = StateMatrix::validateAndSanitize(interB2BDoc());

    CHECK_EQ(m.baseValueCents(), 100000LL);
    CHECK_EQ(m.totalTaxCents(), 18000LL);     // all as IGST
    CHECK(m.tax().has_igst);
    CHECK(!m.tax().has_cgst_sgst);
    CHECK_EQ(m.tax().igst_cents, 18000LL);
    CHECK_EQ(m.tax().cgst_cents, 0LL);
    CHECK(m.supplyType() == SupplyType::INTER);
    CHECK(m.classification() == Classification::B2B);
    CHECK(m.aatoCrore().has_value());
    CHECK_EQ(*m.aatoCrore(), 6.5);
    CHECK(m.irpReportedOn().has_value());
    CHECK(*m.irpReportedOn() == "2026-04-05");
}

TEST(valid_b2c_without_buyer)
{
    const auto m = StateMatrix::validateAndSanitize(b2cDoc());

    CHECK(!m.hasBuyer());
    CHECK(m.classification() == Classification::B2C);
    CHECK(!m.aatoCrore().has_value());        // aato_crore is optional
    CHECK(!m.irnPresent());
    CHECK_EQ(m.tax().cgst_cents, 4500LL);
    CHECK_EQ(m.tax().sgst_cents, 4500LL);
}

// ---- deterministic tax split math -------------------------------------------

TEST(tax_split_intra_halves)
{
    const auto b = StateMatrix::computeTaxSplit(100000LL, 1800, SupplyType::INTRA);
    CHECK_EQ(b.cgst_cents, 9000LL);           // half of 18%
    CHECK_EQ(b.sgst_cents, 9000LL);
    CHECK_EQ(b.igst_cents, 0LL);
    CHECK_EQ(b.total_tax_cents, 18000LL);
    CHECK(b.has_cgst_sgst);
    CHECK(!b.has_igst);
}

TEST(tax_split_inter_full_igst)
{
    const auto b = StateMatrix::computeTaxSplit(100000LL, 1800, SupplyType::INTER);
    CHECK_EQ(b.igst_cents, 18000LL);
    CHECK_EQ(b.cgst_cents, 0LL);
    CHECK_EQ(b.sgst_cents, 0LL);
    CHECK_EQ(b.total_tax_cents, 18000LL);
    CHECK(b.has_igst);
    CHECK(!b.has_cgst_sgst);
}

TEST(tax_split_rounds_half_up_per_head)
{
    // INR 3.00 at 1%: each half-step is 0.015 INR -> rounds to 2 paise each.
    const auto b = StateMatrix::computeTaxSplit(300LL, 100, SupplyType::INTRA);
    CHECK_EQ(b.cgst_cents, 2LL);
    CHECK_EQ(b.sgst_cents, 2LL);
    CHECK_EQ(b.total_tax_cents, 4LL);

    // Sub-paisa amounts collapse to zero (no negative surprises).
    const auto tiny = StateMatrix::computeTaxSplit(1LL, 1800, SupplyType::INTRA);
    CHECK_EQ(tiny.cgst_cents, 0LL);
    CHECK_EQ(tiny.sgst_cents, 0LL);

    // Large base at 9% combined (900 bps): each head is 4.5% -> 89999.991
    // rupees-cents rounds cleanly to a 45000/45000 split with integer math.
    const auto big = StateMatrix::computeTaxSplit(999999LL, 900, SupplyType::INTRA);
    CHECK_EQ(big.cgst_cents, 45000LL);
    CHECK_EQ(big.sgst_cents, 45000LL);
    CHECK_EQ(big.total_tax_cents, 90000LL);
}

TEST(tax_split_rejects_negative_inputs)
{
    CHECK_THROWS(StateMatrix::computeTaxSplit(-1LL, 1800, SupplyType::INTRA),
                 std::invalid_argument);
    CHECK_THROWS(StateMatrix::computeTaxSplit(100LL, -5, SupplyType::INTRA),
                 std::invalid_argument);
}

// ---- snapshot immutability ----------------------------------------------------

TEST(snapshot_is_a_defensive_copy)
{
    const auto m  = StateMatrix::validateAndSanitize(intraDoc());
    const auto snap = m.getSnapshot();

    CHECK_EQ(snap.totalValueCents(), m.totalValueCents());
    CHECK(snap.supplierGstin() == m.supplierGstin());
    // No mutation path exists: the snapshot can never diverge from the
    // original, so equality is total.
    CHECK_EQ(snap.baseValueCents(), m.baseValueCents());
    CHECK(snap.tax() == m.tax());
}

// ---- invalid JSON structures ---------------------------------------------------

TEST(invalid_missing_supplier_throws)
{
    auto doc = interB2BDoc();
    doc.erase("supplier");
    CHECK_THROWS(StateMatrix::validateAndSanitize(doc), std::invalid_argument);
}

TEST(invalid_missing_invoice_throws)
{
    auto doc = interB2BDoc();
    doc.erase("invoice");
    CHECK_THROWS(StateMatrix::validateAndSanitize(doc), std::invalid_argument);
}

TEST(invalid_classification_type_throws)
{
    auto doc = interB2BDoc();
    doc["invoice"]["classification"] = 42;
    CHECK_THROWS(StateMatrix::validateAndSanitize(doc), std::invalid_argument);
}

TEST(invalid_classification_value_throws)
{
    auto doc = interB2BDoc();
    doc["invoice"]["classification"] = "B2X";
    CHECK_THROWS(StateMatrix::validateAndSanitize(doc), std::invalid_argument);
}

TEST(invalid_supply_type_throws)
{
    auto doc = interB2BDoc();
    doc["invoice"]["supply_type"] = "CROSS";
    CHECK_THROWS(StateMatrix::validateAndSanitize(doc), std::invalid_argument);
}

TEST(invalid_money_type_throws)
{
    auto doc = interB2BDoc();
    doc["invoice"]["base_value"] = "one thousand";
    CHECK_THROWS(StateMatrix::validateAndSanitize(doc), std::invalid_argument);
}

TEST(invalid_negative_money_throws)
{
    auto doc = interB2BDoc();
    doc["invoice"]["base_value"] = -1000.0;
    CHECK_THROWS(StateMatrix::validateAndSanitize(doc), std::invalid_argument);
}

TEST(invalid_line_items_not_array_throws)
{
    auto doc = interB2BDoc();
    doc["invoice"]["line_items"] = "nope";
    CHECK_THROWS(StateMatrix::validateAndSanitize(doc), std::invalid_argument);
}

TEST(invalid_missing_irn_when_present_throws)
{
    auto doc = interB2BDoc();
    doc["invoice"].erase("irn");              // irn_present is true but irn absent
    CHECK_THROWS(StateMatrix::validateAndSanitize(doc), std::invalid_argument);
}

TEST(invalid_line_rate_must_be_number)
{
    auto doc = interB2BDoc();
    doc["invoice"]["line_items"][0]["tax_rate"] = "18%";
    CHECK_THROWS(StateMatrix::validateAndSanitize(doc), std::invalid_argument);
}

// ---- generic schema type-walk ---------------------------------------------------

TEST(schema_walk_inline_type_checks)
{
    const json schema = json{{"supplier",
                              json{{"$type", "object"},
                                   {"$required", true},
                                   {"gstin", json{{"$type", "string"}, {"$required", true}}}}}};

    std::vector<std::string> errs;
    CHECK(StateMatrix::validateAgainstSchema(json{{"supplier", json{{"gstin", "27X"}}}},
                                             schema, errs));
    CHECK(errs.empty());

    // Wrong leaf type is reported non-fatally.
    CHECK(!StateMatrix::validateAgainstSchema(json{{"supplier", json{{"gstin", 123}}}},
                                              schema, errs));
    CHECK(!errs.empty());

    // Missing required field is reported.
    CHECK(!StateMatrix::validateAgainstSchema(json{{"supplier", json::object()}},
                                              schema, errs));
}

TEST(schema_walk_against_real_gemini_schema)
{
    std::ifstream in(GEMINI_SCHEMA_PATH);
    if (!in)
    {
        std::printf("  SKIP: cannot open %s\n", GEMINI_SCHEMA_PATH);
        return;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const json schema = json::parse(ss.str());

    std::vector<std::string> errs;

    // A conforming document passes with zero errors.
    CHECK(StateMatrix::validateAgainstSchema(interB2BDoc(), schema, errs));
    CHECK(errs.empty());

    // A type violation surfaces as a named error.
    auto broken = interB2BDoc();
    broken["buyer"]["gstin"] = 99999;         // numeric where string required
    CHECK(!StateMatrix::validateAgainstSchema(broken, schema, errs));
    CHECK(!errs.empty());

    // A missing required top-level field is also reported.
    auto missing = interB2BDoc();
    missing.erase("invoice");
    CHECK(!StateMatrix::validateAgainstSchema(missing, schema, errs));
    CHECK(!errs.empty());
}

} // anonymous namespace

TEST(summary_totals_line_up)
{
    // The aggregate total must equal base + computed tax (cross-check that the
    // fixture's extraction totals are consistent with our split math).
    const auto m = StateMatrix::validateAndSanitize(intraDoc());
    const auto expected = StateMatrix::computeTaxSplit(m.baseValueCents(), 1800,
                                                       SupplyType::INTRA);
    CHECK_EQ(expected.cgst_cents, m.tax().cgst_cents);
    CHECK_EQ(expected.total_tax_cents, m.totalTaxCents());
    CHECK_EQ(m.baseValueCents() + m.totalTaxCents(), m.totalValueCents());
}

TESTS_BEGIN
RUN_TEST(valid_intra_invoice),
RUN_TEST(valid_inter_invoice),
RUN_TEST(valid_b2c_without_buyer),
RUN_TEST(tax_split_intra_halves),
RUN_TEST(tax_split_inter_full_igst),
RUN_TEST(tax_split_rounds_half_up_per_head),
RUN_TEST(tax_split_rejects_negative_inputs),
RUN_TEST(snapshot_is_a_defensive_copy),
RUN_TEST(invalid_missing_supplier_throws),
RUN_TEST(invalid_missing_invoice_throws),
RUN_TEST(invalid_classification_type_throws),
RUN_TEST(invalid_classification_value_throws),
RUN_TEST(invalid_supply_type_throws),
RUN_TEST(invalid_money_type_throws),
RUN_TEST(invalid_negative_money_throws),
RUN_TEST(invalid_line_items_not_array_throws),
RUN_TEST(invalid_missing_irn_when_present_throws),
RUN_TEST(invalid_line_rate_must_be_number),
RUN_TEST(schema_walk_inline_type_checks),
RUN_TEST(schema_walk_against_real_gemini_schema),
RUN_TEST(summary_totals_line_up),
TESTS_END
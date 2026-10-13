// ruleengine_test.cpp — unit tests for the config-driven compliance engine.
//
// Runs the rules from config/rules_2026.json against the shared fixtures and
// targeted mutations, asserting per-rule violation / pass / skip outcomes,
// matrix-derived tax-leg truth, threshold single-sourcing, and message
// template substitution ({token} resolution incl. the hsn alias).

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "RuleEngine.hpp"
#include "StateMatrix.hpp"
#include "fixtures.h"
#include "test_framework.h"

using namespace tax;
using namespace testfix;
using json = nlohmann::json;

namespace {

// Load config/rules_2026.json; returns false when the file is missing so the
// test can report SKIP instead of failing the whole suite.
RuleEngine* buildEngine()
{
    static RuleEngine* cached = nullptr;
    if (cached) return cached;

    std::ifstream in(RULES_CONFIG_PATH);
    if (!in)
    {
        std::printf("  SKIP: cannot open %s\n", RULES_CONFIG_PATH);
        return nullptr;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    cached = new RuleEngine(json::parse(ss.str()));
    return cached;
}

const RuleResult* findRule(const std::vector<RuleResult>& results,
                           const std::string& id)
{
    for (const auto& r : results)
    {
        if (r.id == id) return &r;
    }
    return nullptr;
}

} // anonymous namespace

TEST(engine_parses_real_config)
{
    RuleEngine* eng = buildEngine();
    if (!eng) return;
    // 2026 statutory set defined in config/rules_2026.json.
    CHECK_EQ(eng->ruleCount(), 7u);
}

// ---- R-EINV-001: mandatory e-invoicing above the AATO threshold -------------

TEST(einvoice_rule_passes_with_irn)
{
    RuleEngine* eng = buildEngine();
    if (!eng) return;
    auto res = eng->evaluateDocument(interB2BDoc());   // AATO 6.5, IRN present
    const RuleResult* r = findRule(res, "R-EINV-001");
    CHECK(r && !r->violated);

    res = eng->evaluateDocument(intraDoc());
    r = findRule(res, "R-EINV-001");
    CHECK(r && !r->violated);
}

TEST(einvoice_rule_violated_without_irn)
{
    RuleEngine* eng = buildEngine();
    if (!eng) return;
    auto doc = intraDoc();
    doc["invoice"]["irn_present"] = false;
    doc["invoice"].erase("irn");
    auto res = eng->evaluateDocument(doc);            // AATO 6.5 but no IRN
    const RuleResult* r = findRule(res, "R-EINV-001");
    CHECK(r && r->violated);
    CHECK(r->severity == "error");
    CHECK(r->message.find("6.5") != std::string::npos);  // {aato_crore} resolved
}

TEST(einvoice_rule_below_threshold_passes)
{
    RuleEngine* eng = buildEngine();
    if (!eng) return;
    auto doc = intraDoc();
    doc["supplier"]["aato_crore"] = 4.0;              // under the ₹5 Cr threshold
    doc["invoice"]["irn_present"] = false;
    doc["invoice"].erase("irn");
    auto res = eng->evaluateDocument(doc);
    const RuleResult* r = findRule(res, "R-EINV-001");
    CHECK(r && !r->violated);                         // 'when' fired, aato>5 false
}

TEST(einvoice_rule_skipped_when_aato_unknown)
{
    RuleEngine* eng = buildEngine();
    if (!eng) return;
    auto res = eng->evaluateDocument(b2cDoc());       // aato_crore absent
    const RuleResult* r = findRule(res, "R-EINV-001");
    CHECK(r && r->skipped);
    CHECK(!r->violated);
}

// ---- R-IRP-002: 30-day IRP reporting window ---------------------------------

TEST(irp_rule_passes_when_reported)
{
    RuleEngine* eng = buildEngine();
    if (!eng) return;
    auto res = eng->evaluateDocument(interB2BDoc());  // irn + irp.reported_on
    const RuleResult* r = findRule(res, "R-IRP-002");
    CHECK(r && !r->violated);
}

TEST(irp_rule_violated_when_not_reported)
{
    RuleEngine* eng = buildEngine();
    if (!eng) return;
    auto res = eng->evaluateDocument(intraDoc());     // irn but no irp block
    const RuleResult* r = findRule(res, "R-IRP-002");
    CHECK(r && r->violated);
    CHECK(r->resolve_gap_by == "irp_reported_on");
}

TEST(irp_rule_skipped_without_irn)
{
    RuleEngine* eng = buildEngine();
    if (!eng) return;
    auto res = eng->evaluateDocument(b2cDoc());       // irn_present false
    const RuleResult* r = findRule(res, "R-IRP-002");
    CHECK(r && r->skipped);
}

// ---- R-SPLIT-003/-004: intra vs inter tax allocation ------------------------

TEST(intra_igst_rule_passes_on_consistent_docs)
{
    RuleEngine* eng = buildEngine();
    if (!eng) return;
    auto res = eng->evaluateDocument(intraDoc());     // intra, CGST+SGST only
    const RuleResult* r = findRule(res, "R-SPLIT-003");
    CHECK(r && !r->violated);

    res = eng->evaluateDocument(interB2BDoc());       // inter-state => IGST fine
    r = findRule(res, "R-SPLIT-003");
    CHECK(r && !r->violated);
}

TEST(intra_igst_rule_violated_when_intra_carries_igst)
{
    RuleEngine* eng = buildEngine();
    if (!eng) return;
    auto doc = intraDoc();
    doc["invoice"]["tax_breakdown"] = igstOnly(180.0);   // intra but IGST 180
    auto res = eng->evaluateDocument(doc);
    const RuleResult* r = findRule(res, "R-SPLIT-003");
    CHECK(r && r->violated);
}

TEST(intra_igst_rule_uses_matrix_derived_truth)
{
    RuleEngine* eng = buildEngine();
    if (!eng) return;
    // Payload LIES: claims has_igst=false while carrying IGST 180. The rule
    // must trust the amounts, not the booleans ("truth dwells in the amounts").
    auto doc = intraDoc();
    auto& tb = doc["invoice"]["tax_breakdown"];
    tb["cgst"] = 0.0; tb["sgst"] = 0.0; tb["igst"] = 180.0;
    tb["total_tax"] = 180.0;
    tb["has_cgst_sgst"] = false;
    tb["has_igst"] = false;                            // <-- the lie
    auto res = eng->evaluateDocument(doc);
    const RuleResult* r = findRule(res, "R-SPLIT-003");
    CHECK(r && r->violated);
}

TEST(inter_cgst_rule_passes_on_consistent_docs)
{
    RuleEngine* eng = buildEngine();
    if (!eng) return;
    auto res = eng->evaluateDocument(interB2BDoc());  // inter, IGST only
    const RuleResult* r = findRule(res, "R-SPLIT-004");
    CHECK(r && !r->violated);

    res = eng->evaluateDocument(intraDoc());
    r = findRule(res, "R-SPLIT-004");
    CHECK(r && !r->violated);
}

TEST(inter_cgst_rule_violated_when_inter_carries_cgst_sgst)
{
    RuleEngine* eng = buildEngine();
    if (!eng) return;
    auto doc = interB2BDoc();
    doc["invoice"]["tax_breakdown"] = cgstSgst(90.0, 90.0);   // inter but CGST+SGST
    auto res = eng->evaluateDocument(doc);
    const RuleResult* r = findRule(res, "R-SPLIT-004");
    CHECK(r && r->violated);
}

// ---- R-HSN-005: HSN length must be 4 or 6 digits ----------------------------

TEST(hsn_rule_passes_for_eight_digit_code)
{
    RuleEngine* eng = buildEngine();
    if (!eng) return;
    // 8-digit HSNs (laptop "84713000") are valid — the config now allows
    // 4/6/8 digits (8-digit export / high-turnover codes).
    auto res = eng->evaluateDocument(interB2BDoc());
    const RuleResult* r = findRule(res, "R-HSN-005");
    CHECK(r && !r->violated);
}

TEST(hsn_rule_flags_seven_digit_code)
{
    RuleEngine* eng = buildEngine();
    if (!eng) return;
    auto doc = interB2BDoc();
    doc["invoice"]["hsn_string"] = "8471301";      // 7 digits: outside 4/6/8
    auto res = eng->evaluateDocument(doc);
    const RuleResult* r = findRule(res, "R-HSN-005");
    CHECK(r && r->violated);
    CHECK(r->severity == "warning");
    CHECK(r->message.find("8471301") != std::string::npos);    // {hsn} alias
    // {line_index} is not resolvable from the invoice-level hsn_string fact,
    // so it stays verbatim rather than crashing or fabricating a value.
    CHECK(r->message.find("{line_index}") != std::string::npos);
}

TEST(hsn_rule_passes_for_four_and_six_digit_codes)
{
    RuleEngine* eng = buildEngine();
    if (!eng) return;

    auto doc4 = interB2BDoc();
    doc4["invoice"]["hsn_string"] = "8471";
    auto res = eng->evaluateDocument(doc4);
    const RuleResult* r = findRule(res, "R-HSN-005");
    CHECK(r && !r->violated);

    auto doc6 = interB2BDoc();
    doc6["invoice"]["hsn_string"] = "847130";
    res = eng->evaluateDocument(doc6);
    r = findRule(res, "R-HSN-005");
    CHECK(r && !r->violated);
}

// ---- R-CHRG-006: intra-state SGST must equal CGST ---------------------------

TEST(charge_rule_passes_when_splits_match)
{
    RuleEngine* eng = buildEngine();
    if (!eng) return;
    auto res = eng->evaluateDocument(intraDoc());     // 90/90
    const RuleResult* r = findRule(res, "R-CHRG-006");
    CHECK(r && !r->violated);
}

TEST(charge_rule_violated_on_asymmetric_split)
{
    RuleEngine* eng = buildEngine();
    if (!eng) return;
    auto doc = intraDoc();
    doc["invoice"]["tax_breakdown"]["sgst"] = 91.0;   // 90 vs 91
    doc["invoice"]["tax_breakdown"]["total_tax"] = 181.0;
    auto res = eng->evaluateDocument(doc);
    const RuleResult* r = findRule(res, "R-CHRG-006");
    CHECK(r && r->violated);
    // Template resolved {cgst}/{sgst} into the message body.
    CHECK(r->message.find("(91)") != std::string::npos);
    CHECK(r->message.find("(90)") != std::string::npos);
}

TEST(charge_rule_violated_when_head_missing)
{
    RuleEngine* eng = buildEngine();
    if (!eng) return;
    auto doc = intraDoc();
    doc["invoice"]["tax_breakdown"].erase("cgst");    // cgst absent
    auto res = eng->evaluateDocument(doc);
    const RuleResult* r = findRule(res, "R-CHRG-006");
    CHECK(r && r->violated);
}

TEST(charge_rule_skipped_on_inter_state)
{
    RuleEngine* eng = buildEngine();
    if (!eng) return;
    auto res = eng->evaluateDocument(interB2BDoc());  // has_igst true
    const RuleResult* r = findRule(res, "R-CHRG-006");
    CHECK(r && r->skipped);
    CHECK(!r->violated);
}

// ---- R-CLASS-007: B2B/B2C determination -------------------------------------

TEST(classification_rule_passes_when_b2b_determined)
{
    RuleEngine* eng = buildEngine();
    if (!eng) return;
    auto res = eng->evaluateDocument(interB2BDoc());  // B2B + buyer GSTIN
    const RuleResult* r = findRule(res, "R-CLASS-007");
    CHECK(r && !r->violated);
}

TEST(classification_rule_violated_when_undetermined)
{
    RuleEngine* eng = buildEngine();
    if (!eng) return;
    auto doc = interB2BDoc();
    doc["invoice"]["classification"] = "UNDETERMINED";
    auto res = eng->evaluateDocument(doc);
    const RuleResult* r = findRule(res, "R-CLASS-007");
    CHECK(r && r->violated);
}

TEST(classification_rule_passes_for_legit_b2c)
{
    RuleEngine* eng = buildEngine();
    if (!eng) return;
    // R-CLASS-007 tightened: a legitimately-classified B2C sale (no buyer
    // GSTIN) is no longer flagged — the missing-GSTIN group now requires the
    // classification NOT to be B2C.
    auto res = eng->evaluateDocument(b2cDoc());
    const RuleResult* r = findRule(res, "R-CLASS-007");
    CHECK(r && !r->violated);
    CHECK(!r->skipped);
}

TEST(classification_rule_violated_when_b2b_missing_buyer_gstin)
{
    RuleEngine* eng = buildEngine();
    if (!eng) return;
    auto doc = intraDoc();                         // B2B classification
    doc.erase("buyer");                            // but no buyer GSTIN
    auto res = eng->evaluateDocument(doc);
    const RuleResult* r = findRule(res, "R-CLASS-007");
    CHECK(r && r->violated);
}

// ---- Validation gate + threshold single-sourcing -----------------------------

TEST(evaluate_document_validates_before_rules)
{
    RuleEngine* eng = buildEngine();
    if (!eng) return;
    auto doc = interB2BDoc();
    doc.erase("invoice");                              // structurally invalid
    CHECK_THROWS(eng->evaluateDocument(doc), std::invalid_argument);
}

TEST(aato_threshold_is_config_driven)
{
    RuleEngine* eng = buildEngine();
    if (!eng) return;
    // 4.0 crore is under the configured threshold (5) -> no violation. The
    // threshold JSON ("source": "thresholds.e_invoicing_aato_crore") drives it.
    auto doc = intraDoc();
    doc["supplier"]["aato_crore"] = 4.9;
    auto res = eng->evaluateDocument(doc);
    const RuleResult* r = findRule(res, "R-EINV-001");
    CHECK(r && !r->violated);
}

TESTS_BEGIN
RUN_TEST(engine_parses_real_config),
RUN_TEST(einvoice_rule_passes_with_irn),
RUN_TEST(einvoice_rule_violated_without_irn),
RUN_TEST(einvoice_rule_below_threshold_passes),
RUN_TEST(einvoice_rule_skipped_when_aato_unknown),
RUN_TEST(irp_rule_passes_when_reported),
RUN_TEST(irp_rule_violated_when_not_reported),
RUN_TEST(irp_rule_skipped_without_irn),
RUN_TEST(intra_igst_rule_passes_on_consistent_docs),
RUN_TEST(intra_igst_rule_violated_when_intra_carries_igst),
RUN_TEST(intra_igst_rule_uses_matrix_derived_truth),
RUN_TEST(inter_cgst_rule_passes_on_consistent_docs),
RUN_TEST(inter_cgst_rule_violated_when_inter_carries_cgst_sgst),
RUN_TEST(hsn_rule_passes_for_eight_digit_code),
RUN_TEST(hsn_rule_passes_for_four_and_six_digit_codes),
RUN_TEST(hsn_rule_flags_seven_digit_code),
RUN_TEST(charge_rule_passes_when_splits_match),
RUN_TEST(charge_rule_violated_on_asymmetric_split),
RUN_TEST(charge_rule_violated_when_head_missing),
RUN_TEST(charge_rule_skipped_on_inter_state),
RUN_TEST(classification_rule_passes_when_b2b_determined),
RUN_TEST(classification_rule_violated_when_undetermined),
RUN_TEST(classification_rule_passes_for_legit_b2c),
RUN_TEST(classification_rule_violated_when_b2b_missing_buyer_gstin),
RUN_TEST(evaluate_document_validates_before_rules),
RUN_TEST(aato_threshold_is_config_driven),
TESTS_END
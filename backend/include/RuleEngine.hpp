// ============================================================================
// RuleEngine — config-driven 2026 GST compliance verification.
//
// Reads the declarative rule set (config/rules_2026.json) and evaluates it
// against an immutable StateMatrix plus the sanitized source document. Rules
// are data: editing the JSON never requires recompiling the binary.
//
// Semantics (mirrors the "$condition_dsl" block in rules_2026.json):
//   * Every condition is a VIOLATION predicate: TRUE => the rule is violated
//     and a gap is raised; FALSE => the rule passes.
//   * An optional "when" precondition gates the whole rule: when it does not
//     fire the rule is SKIPPED (reported as a non-violation with skipped=true).
//   * Multiple checks combine with "operator": AND (all must fire) or OR (any).
//   * Fact paths resolve against the sanitized document — dot-separated
//     object keys with optional "[n]" array-index segments. Two paths are
//     special: invoice.tax_breakdown.has_igst / has_cgst_sgst resolve to the
//     MATRIX-DERIVED truth (derived from amounts, never the claimed booleans),
//     so a payload that lies about its tax legs still trips the rules.
//   * Operator aliases: "gt"/"gte"/"lt"/"lte" are accepted for ">"/">="/<"/"<="
//     to keep the JSON readable. "token_length_out" is a VALIDATION predicate:
//     it fires (violation) when the token's length is OUTSIDE the declared set
//     — R-HSN-005 ["values":[4,6,8]] means a violation for any HSN that is not
//     4, 6 or 8 digits.
//   * A "check" may instead be a nested composite {operator, checks:[...]} with
//     no "field". It is evaluated recursively and counts as one check, so the
//     rule DSL can express grouped conditions — e.g. R-CLASS-007 flags a missing
//     buyer GSTIN only when the classification is not B2C (AND group).
//   * A check may carry "source" naming a threshold ("thresholds.<key>");
//     when resolvable it overrides the literal "value", keeping the rules
//     single-sourced on config/thresholds.
//   * Message templates substitute {token} placeholders from the extracted
//     document (bare leaf names, full dotted paths, or the engine's alias
//     table). Unresolvable tokens are left verbatim.
//
// This engine never mutates the matrix and performs no finance math. It reads
// facts and reports gaps that the chatbot layer then resolves.
// ============================================================================

#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "json.hpp"
#include "StateMatrix.hpp"

namespace tax {

// Verdict for a single rule after evaluating one document.
struct RuleResult
{
    std::string id;
    std::string category;
    std::string severity;            // "error" | "warning" | "info"
    std::string status_field;
    std::string summary;

    bool        violated = false;    // condition DSL evaluated TRUE => gap raised
    bool        skipped  = false;    // "when" precondition never fired

    std::string message;             // substituted message_template (violated only)
    std::string resolve_gap_by;      // which clarification the chat flow emits
    std::string action_required;     // INPUT_TEXT / ...

    std::vector<std::string> fired_checks;  // human-readable triggers (violated only)
};

class RuleEngine
{
public:
    // Accepts either the full config document ({...,"rules":[...]}) or a bare
    // rules array. Throws std::invalid_argument on malformed rule structure.
    explicit RuleEngine(const json& rules_config);

    // Evaluate every rule, in config order, against a validated matrix and the
    // sanitized document it was built from. Both inputs are read-only.
    std::vector<RuleResult> evaluate(const StateMatrix& matrix,
                                     const json& raw_doc) const;

    // Validate (StateMatrix::validateAndSanitize) then evaluate in one step.
    // Throws std::invalid_argument when the document is structurally invalid.
    std::vector<RuleResult> evaluateDocument(const json& raw_doc) const;

    std::size_t ruleCount() const noexcept { return rules_.size(); }

private:
    // ------------------------------------------------------------------ data
    struct RuleSpec
    {
        std::string id;
        std::string category;
        std::string severity;
        std::string status_field;
        std::string summary;
        std::string resolve_gap_by;
        std::string action_required;
        std::string message_template;
        json        condition;             // { operator, when?, checks:[...] }
    };
    std::vector<RuleSpec>                rules_;
    json                                 thresholds_;
    std::map<std::string, std::string>   template_aliases_;

    // ------------------------------------------------------------ evaluation
    // Resolve a DSL fact path against the raw document. Dot-separated object
    // keys; a segment may carry a "[n]" array-index suffix. Returns nullopt
    // when any segment is missing or mis-typed (the "absent" state).
    static std::optional<json> resolvePath(const json& doc, const std::string& path);

    // Fact resolution with matrix-derived override for the tax-leg flags.
    std::optional<json> resolveFact(const StateMatrix& matrix, const json& doc,
                                    const std::string& path) const;

    // Verdict for one check (or nested group). `label` names the check for
    // diagnostics; `fired` is the violation-predicate outcome.
    struct CheckOutcome
    {
        bool        fired = false;
        std::string label;
    };

    // Evaluate ONE check (or a nested composite group) as a violation
    // predicate (true => this check fired).
    CheckOutcome evalCheck(const json& check, const StateMatrix& matrix,
                           const json& doc) const;

    // Evaluate one rule end-to-end into a RuleResult.
    RuleResult evalRule(const RuleSpec& spec, const StateMatrix& matrix,
                        const json& doc) const;

    // ---- thresholds ---------------------------------------------------------
    // "thresholds.<key>" dotted path -> threshold value; nullopt when the
    // source string does not denote a resolvable threshold (e.g. it is prose).
    std::optional<json> resolveThreshold(const std::string& source) const;

    // ---- message template substitution --------------------------------------
    struct FactTable
    {
        std::map<std::string, json> full;  // dotted path -> leaf
        std::map<std::string, json> bare;  // bare leaf name -> leaf (last wins)
    };
    // Walk the document once, flattening every scalar leaf into the table.
    static void collectFacts(const json& node, const std::string& prefix,
                             FactTable& table);
    std::string substitute(const std::string& tpl, const FactTable& table,
                           const json& doc) const;
    static std::string formatValue(const json& value);
};

} // namespace tax
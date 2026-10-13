// ============================================================================
// RuleEngine.cpp — config-driven rule evaluation (see RuleEngine.hpp).
// Pure decision logic: reads an immutable StateMatrix + sanitized document,
// produces RuleResults. No network, DB, file, or UI access.
// ============================================================================

#include "RuleEngine.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace tax {

namespace {

// Keep the violation-predicate story explicit for the numeric ops: a fact that
// is ABSENT never 'fires' a numeric/string comparison (we do not fabricate
// gaps for facts — an absent fact is reported only by the 'absent' op).
bool scalarLess(const json& a, const json& b, std::string op)
{
    if (!a.is_number() || !b.is_number()) return false;
    const double x = a.get<double>();
    const double y = b.get<double>();
    if (op == ">" ) return x >  y;
    if (op == ">=") return x >= y;
    if (op == "<" ) return x <  y;
    if (op == "<=") return x <= y;
    return false;
}

bool valuesEqualAsScalar(const json& a, const json& b)
{
    if (a.is_number() && b.is_number()) return a.get<double>() == b.get<double>();
    if (a.is_string() && b.is_string()) return a.get<std::string>() == b.get<std::string>();
    return a == b;
}

// Split a dot-path segment into its key and optional array index, e.g.
// "line_items[0]" -> ("line_items", 0).
std::string splitSegment(const std::string& seg, std::optional<std::size_t>& index)
{
    const std::size_t br = seg.find('[');
    if (br == std::string::npos)
    {
        index.reset();
        return seg;
    }
    const std::string key   = seg.substr(0, br);
    const std::string inside = seg.substr(br + 1, seg.size() - br - 2);
    index = static_cast<std::size_t>(std::strtoul(inside.c_str(), nullptr, 10));
    return key;
}

} // anonymous namespace

// ===========================================================================
// Construction
// ===========================================================================

RuleEngine::RuleEngine(const json& rules_config)
{
    // Accept {..., "rules": [...]} or a bare rules array.
    const json* rules = nullptr;
    if (rules_config.is_array())
    {
        rules = &rules_config;
    }
    else if (rules_config.is_object() && rules_config.contains("rules"))
    {
        rules = &rules_config["rules"];
    }
    if (!rules || !rules->is_array())
    {
        throw std::invalid_argument("RuleEngine: config must be {...,\"rules\":[...]} "
                                    "or a bare rules array");
    }

    if (rules_config.is_object() && rules_config.contains("thresholds"))
    {
        thresholds_ = rules_config["thresholds"];
    }

    // Default token->path aliases so templates like "{hsn}" resolve to the
    // extracted invoice.hsn_string even though the DSL field names differ.
    template_aliases_ = {
        {"hsn", "invoice.hsn_string"},
        {"aato_crore", "supplier.aato_crore"},
        {"cgst", "invoice.tax_breakdown.cgst"},
        {"sgst", "invoice.tax_breakdown.sgst"},
        {"igst", "invoice.tax_breakdown.igst"},
        {"total_tax", "invoice.tax_breakdown.total_tax"},
        {"irn", "invoice.irn"},
    };

    rules_.reserve(rules->size());
    for (const auto& r : *rules)
    {
        if (!r.is_object())
        {
            throw std::invalid_argument("RuleEngine: each rule must be an object");
        }
        RuleSpec spec;
        spec.id            = r.value("id", "");
        spec.category      = r.value("category", "");
        spec.severity      = r.value("severity", "");
        spec.status_field  = r.value("status_field", "");
        spec.summary       = r.value("summary", "");
        spec.resolve_gap_by     = r.value("resolve_gap_by", "");
        spec.action_required    = r.value("action_required", "");
        spec.message_template   = r.value("message_template", "");
        if (spec.id.empty())
        {
            throw std::invalid_argument("RuleEngine: rule missing required 'id'");
        }
        if (!r.contains("condition") || !r["condition"].is_object())
        {
            throw std::invalid_argument("RuleEngine: rule " + spec.id +
                                        " missing required 'condition' object");
        }
        spec.condition = r["condition"];
        if (spec.condition.contains("operator"))
        {
            const std::string op = spec.condition["operator"].get<std::string>();
            if (op != "AND" && op != "OR")
            {
                throw std::invalid_argument("RuleEngine: rule " + spec.id +
                                            " condition.operator must be AND or OR");
            }
        }
        if (spec.condition.contains("checks") &&
            !spec.condition["checks"].is_array())
        {
            throw std::invalid_argument("RuleEngine: rule " + spec.id +
                                        " condition.checks must be an array");
        }
        if (spec.condition.contains("when") &&
            !spec.condition["when"].is_object())
        {
            throw std::invalid_argument("RuleEngine: rule " + spec.id +
                                        " condition.when must be an object");
        }
        rules_.push_back(std::move(spec));
    }
}

// ===========================================================================
// Entry points
// ===========================================================================

std::vector<RuleResult> RuleEngine::evaluate(const StateMatrix& matrix,
                                             const json& raw_doc) const
{
    std::vector<RuleResult> results;
    results.reserve(rules_.size());
    for (const auto& spec : rules_)
    {
        results.push_back(evalRule(spec, matrix, raw_doc));
    }
    return results;
}

std::vector<RuleResult> RuleEngine::evaluateDocument(const json& raw_doc) const
{
    return evaluate(StateMatrix::validateAndSanitize(raw_doc), raw_doc);
}

// ===========================================================================
// Path resolution
// ===========================================================================

std::optional<json> RuleEngine::resolvePath(const json& doc, const std::string& path)
{
    const json* cur = &doc;
    std::size_t start = 0;
    while (true)
    {
        const std::size_t dot = path.find('.', start);
        const std::string seg = path.substr(start,
                                            dot == std::string::npos
                                                ? std::string::npos
                                                : dot - start);
        std::optional<std::size_t> index;
        const std::string key = splitSegment(seg, index);

        if (index.has_value())
        {
            if (!cur->is_array() || index.value() >= cur->size())
            {
                return std::nullopt;
            }
            cur = &(*cur)[index.value()];
        }
        else
        {
            if (!cur->is_object() || !cur->contains(key))
            {
                return std::nullopt;
            }
            cur = &(*cur)[key];
        }

        if (dot == std::string::npos)
        {
            return *cur;   // leaf found; return a copy
        }
        start = dot + 1;
    }
}

std::optional<json> RuleEngine::resolveFact(const StateMatrix& matrix,
                                            const json& doc,
                                            const std::string& path) const
{
    // Matrix-derived truth wins over any claimed boolean in the payload: an
    // invoice that (incorrectly) declares has_igst=false while carrying IGST
    // is still treated as an IGST-bearing invoice by the rules.
    if (path == "invoice.tax_breakdown.has_igst")
    {
        return json(matrix.tax().has_igst);
    }
    if (path == "invoice.tax_breakdown.has_cgst_sgst")
    {
        return json(matrix.tax().has_cgst_sgst);
    }
    return resolvePath(doc, path);
}

// ===========================================================================
// Check evaluation (violation predicate)
// ===========================================================================

RuleEngine::CheckOutcome RuleEngine::evalCheck(const json& check,
                                               const StateMatrix& matrix,
                                               const json& doc) const
{
    if (!check.is_object())
    {
        return {false, "?"};   // malformed: never fabricates a violation
    }

    // ---- nested composite group {operator, checks:[...]} ------------------
    // A check with no "field" but an operator + checks is grouped condition.
    if (!check.contains("field") && check.contains("operator") &&
        check.contains("checks") && check["checks"].is_array())
    {
        const std::string gop = check["operator"].get<std::string>();
        std::vector<std::string> labels;
        labels.reserve(check["checks"].size());
        bool fired = true;                       // AND: all must fire
        if (gop == "OR") fired = false;          // OR:    any may fire
        for (const auto& c : check["checks"])
        {
            const CheckOutcome o = evalCheck(c, matrix, doc);
            labels.push_back(o.label);
            if (gop == "OR")      fired = fired || o.fired;
            else                 fired = fired && o.fired;
        }
        if (check["checks"].empty()) fired = false;   // empty group always passes

        std::string label = "( ";
        for (std::size_t i = 0; i < labels.size(); ++i)
        {
            if (i) label += ' ' + gop + ' ';
            label += labels[i];
        }
        label += " )";
        return {fired, std::move(label)};
    }

    if (!check.contains("field") || !check["field"].is_string())
    {
        return {false, "?"};
    }
    const std::string field = check["field"].get<std::string>();
    std::string op    = check.value("op", "");

    // Canonical arithmetic spellings; the JSON may write gt/gte/lt/lte.
    if (op == "gt")  op = ">";
    if (op == "gte") op = ">=";
    if (op == "lt")  op = "<";
    if (op == "lte") op = "<=";

    const auto fact         = resolveFact(matrix, doc, field);
    const std::string label = field + " " + op;

    // Threshold single-sourcing: when "source" denotes a resolvable threshold
    // it overrides a literal "value". Prose sources are ignored.
    json lit;
    if (check.contains("value")) lit = check["value"];
    if (check.contains("source") && check["source"].is_string())
    {
        if (auto t = resolveThreshold(check["source"].get<std::string>()))
        {
            lit = *t;
        }
    }

    if (op == "present") return {fact.has_value(), label};
    if (op == "absent")  return {!fact.has_value(), label};
    if (op == "eq_true" || op == "eq_false")
    {
        if (!fact.has_value() || !fact->is_boolean()) return {false, label};
        return {op == "eq_true" ? fact->get<bool>() : !fact->get<bool>(), label};
    }
    if (op == "token_length_out")
    {
        // Validation predicate: fires when the token's length is OUTSIDE the
        // declared set (R-HSN-005: 4/6/8 digits). Absent/non-string passes.
        if (!fact.has_value() || !fact->is_string() ||
            !check.contains("values") || !check["values"].is_array())
        {
            return {false, label};
        }
        const std::size_t len = fact->get<std::string>().size();
        bool allowed = false;
        for (const auto& v : check["values"])
        {
            if (v.is_number() &&
                static_cast<std::size_t>(v.get<double>()) == len)
            {
                allowed = true;
                break;
            }
        }
        return {!allowed, label};
    }
    if (op == "ne_cross")
    {
        if (!check.contains("value_from") || !check["value_from"].is_string())
        {
            return {false, label};
        }
        const auto other = resolveFact(matrix, doc,
                                       check["value_from"].get<std::string>());
        if (!fact.has_value() || !other.has_value()) return {false, label};
        return {!valuesEqualAsScalar(*fact, *other), label};
    }
    if (op == "eq" || op == "neq")
    {
        if (!fact.has_value()) return {false, label};   // absent never compares
        const bool eq = valuesEqualAsScalar(*fact, lit);
        return {op == "eq" ? eq : !eq, label};
    }
    // arithmetic: > >= < <=
    if (op == ">" || op == ">=" || op == "<" || op == "<=")
    {
        if (!fact.has_value() || !lit.is_number()) return {false, label};
        return {scalarLess(*fact, lit, op), label};
    }
    return {false, label + " (unknown op)"};   // unknown op: conservative pass
}

// ===========================================================================
// Threshold lookup
// ===========================================================================

std::optional<json> RuleEngine::resolveThreshold(const std::string& source) const
{
    // Only pure dotted paths under "thresholds." qualify. Reject prose values
    // like "thresholds.hsn_min_digits / hsn_max_digits".
    if (source.rfind("thresholds.", 0) != 0) return std::nullopt;
    const std::string rest = source.substr(std::strlen("thresholds."));
    if (rest.find('/') != std::string::npos ||
        rest.find(' ') != std::string::npos ||
        rest.empty())
    {
        return std::nullopt;
    }
    return resolvePath(thresholds_, rest);
}

// ===========================================================================
// Message template substitution
// ===========================================================================

void RuleEngine::collectFacts(const json& node, const std::string& prefix,
                              FactTable& table)
{
    if (node.is_object())
    {
        for (auto it = node.begin(); it != node.end(); ++it)
        {
            const std::string path =
                prefix.empty() ? it.key() : prefix + "." + it.key();
            if (it.value().is_object() || it.value().is_array())
            {
                collectFacts(it.value(), path, table);
            }
            else
            {
                table.full[path] = it.value();
                table.bare[it.key()] = it.value();
            }
        }
    }
    else if (node.is_array())
    {
        for (std::size_t i = 0; i < node.size(); ++i)
        {
            const std::string path = prefix + "[" + std::to_string(i) + "]";
            if (node[i].is_object() || node[i].is_array())
            {
                collectFacts(node[i], path, table);
            }
            else
            {
                table.full[path] = node[i];
            }
        }
    }
}

std::string RuleEngine::formatValue(const json& value)
{
    if (value.is_string())     return value.get<std::string>();
    if (value.is_boolean())    return value.get<bool>() ? "true" : "false";
    if (value.is_number_integer()) return std::to_string(value.get<std::int64_t>());
    if (value.is_number_float())
    {
        const double d = value.get<double>();
        if (d == static_cast<double>(static_cast<long long>(d)))
        {
            return std::to_string(static_cast<long long>(d));   // 90.0 -> "90"
        }
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.6g", d);
        return buf;
    }
    return value.dump();   // arrays / objects (unlikely in a template)
}

std::string RuleEngine::substitute(const std::string& tpl, const FactTable& table,
                                   const json& doc) const
{
    std::string out;
    out.reserve(tpl.size());
    for (std::size_t i = 0; i < tpl.size(); ++i)
    {
        if (tpl[i] != '{')
        {
            out.push_back(tpl[i]);
            continue;
        }
        const std::size_t close = tpl.find('}', i + 1);
        if (close == std::string::npos)
        {
            out.append(tpl, i, std::string::npos);   // dangling brace: copy rest
            break;
        }
        const std::string token = tpl.substr(i + 1, close - i - 1);
        std::optional<json> resolved;
        // Resolution order: exact full dotted path, then the deliberate
        // template alias (e.g. {hsn} -> invoice.hsn_string), then the bare
        // last-leaf-wins name. Alias-first guarantees {hsn} binds the
        // invoice-level HSN even when a line item also has a leaf named "hsn".
        auto fit = table.full.find(token);
        if (fit != table.full.end())
        {
            resolved = fit->second;
        }
        else
        {
            auto ait = template_aliases_.find(token);
            if (ait != template_aliases_.end())
            {
                resolved = resolvePath(doc, ait->second);
            }
            else
            {
                auto bit = table.bare.find(token);
                if (bit != table.bare.end())
                {
                    resolved = bit->second;
                }
            }
        }
        if (resolved.has_value())
        {
            out += formatValue(*resolved);
        }
        else
        {
            out += '{' + token + '}';   // unresolvable token stays verbatim
        }
        i = close;
    }
    return out;
}

// ===========================================================================
// Single-rule evaluation
// ===========================================================================

RuleResult RuleEngine::evalRule(const RuleSpec& spec, const StateMatrix& matrix,
                                const json& doc) const
{
    RuleResult result;
    result.id             = spec.id;
    result.category       = spec.category;
    result.severity       = spec.severity;
    result.status_field   = spec.status_field;
    result.summary        = spec.summary;
    result.resolve_gap_by = spec.resolve_gap_by;
    result.action_required= spec.action_required;

    const json& cond   = spec.condition;
    const std::string op = cond.value("operator", "AND");

    // "when" precondition: it does not fire => the whole rule is skipped.
    if (cond.contains("when"))
    {
        const bool gated = evalCheck(cond["when"], matrix, doc).fired;
        if (!gated)
        {
            result.skipped = true;
            return result;
        }
    }

    // Combine the per-check violation predicates (checks may be nested groups).
    bool violated = false;
    std::vector<std::string> fired;
    if (cond.contains("checks") && cond["checks"].is_array())
    {
        const json& checks = cond["checks"];
        if (op == "OR")
        {
            for (const auto& c : checks)
            {
                const CheckOutcome o = evalCheck(c, matrix, doc);
                if (o.fired)
                {
                    violated = true;
                    fired.push_back(o.label);
                }
            }
        }
        else   // AND: violated only when EVERY check fires (empty body passes)
        {
            violated = !checks.empty();
            for (const auto& c : checks)
            {
                const CheckOutcome o = evalCheck(c, matrix, doc);
                if (!o.fired) violated = false;
                fired.push_back(o.label);
            }
        }
    }

    result.violated = violated;
    if (violated) result.fired_checks = std::move(fired);
    if (violated && !spec.message_template.empty())
    {
        FactTable table;
        collectFacts(doc, "", table);
        // Ensure the matrix-derived flags reach the template substitution too.
        table.full["invoice.tax_breakdown.has_igst"]        = json(matrix.tax().has_igst);
        table.full["invoice.tax_breakdown.has_cgst_sgst"]   = json(matrix.tax().has_cgst_sgst);
        table.bare["has_igst"]       = json(matrix.tax().has_igst);
        table.bare["has_cgst_sgst"]  = json(matrix.tax().has_cgst_sgst);
        result.message = substitute(spec.message_template, table, doc);
    }
    return result;
}

} // namespace tax
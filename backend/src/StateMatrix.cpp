// ============================================================================
// StateMatrix.cpp — immutable tax ledger + deterministic integer-cent math.
// See StateMatrix.hpp for the contract. Rule evaluation and GSTR serialization
// consume this; nothing here talks to the network, database, or frontend.
// ============================================================================

#include "StateMatrix.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace tax {
namespace {

// ---------------------------------------------------------------------------
// Low-level primitives shared by validateAndSanitize and validateAgainstSchema
// ---------------------------------------------------------------------------

const std::string& requireString(const json& value, const std::string& path)
{
    if (!value.is_string())
    {
        throw std::invalid_argument("StateMatrix: " + path + ": expected a string");
    }
    return value.get_ref<const std::string&>();
}

// Scale the JSON rate (percent) to integer basis points (18.0 -> 1800).
int rateToBps(const json& value, const std::string& path)
{
    if (!value.is_number())
    {
        throw std::invalid_argument("StateMatrix: " + path + ": expected a number");
    }
    const double pct = value.get<double>();
    if (!std::isfinite(pct) || pct < 0.0)
    {
        throw std::invalid_argument("StateMatrix: " + path + ": rate must be finite and non-negative");
    }
    const double bps = pct * 100.0;
    if (bps > static_cast<double>(std::numeric_limits<int>::max()))
    {
        throw std::invalid_argument("StateMatrix: " + path + ": rate out of range");
    }
    return static_cast<int>(std::llround(bps));
}

} // anonymous namespace

// ===========================================================================
// money
// ===========================================================================

Cents money::toCents(const json& value)
{
    if (!value.is_number())
    {
        throw std::invalid_argument("StateMatrix: money value is not a number");
    }
    const double d = value.get<double>();
    if (!std::isfinite(d))
    {
        throw std::invalid_argument("StateMatrix: money value is not finite");
    }
    const double scaled = d * 100.0;
    if (scaled < 0.0 || scaled > 1.0e15)
    {
        throw std::invalid_argument("StateMatrix: money value out of range (negative or too large)");
    }
    return static_cast<Cents>(std::llround(scaled));
}

Cents money::taxOn(Cents base_cents, int rate_bps)
{
    if (base_cents < 0)
    {
        throw std::invalid_argument("StateMatrix: taxOn base must be non-negative");
    }
    if (rate_bps < 0)
    {
        throw std::invalid_argument("StateMatrix: taxOn rate must be non-negative");
    }
    // Guard the multiply: base_cents * rate_bps must fit in Cents plus the
    // +5000 half-up bias.
    if (base_cents != 0 &&
        rate_bps > (std::numeric_limits<Cents>::max() - 5000 - 1) / base_cents)
    {
        throw std::overflow_error("StateMatrix: taxOn overflow");
    }
    // Round-half-up to the nearest cent (paisa): /10000 because the divisor is
    // (rate_bps where 1% == 100 bps, over a cents base == rupees*100).
    return (base_cents * static_cast<Cents>(rate_bps) + 5000) / 10000;
}

// ===========================================================================
// Public API
// ===========================================================================

TaxBreakdown StateMatrix::computeTaxSplit(Cents base_cents, int tax_rate_bps,
                                          SupplyType type)
{
    TaxBreakdown b;
    switch (type)
    {
    case SupplyType::INTRA:
        // CGST and SGST each carry exactly half the combined statutory rate.
        // Per-head independent rounding mirrors the GSTN rule.
        b.cgst_cents        = money::taxOn(base_cents, tax_rate_bps / 2);
        b.sgst_cents        = money::taxOn(base_cents, tax_rate_bps / 2);
        b.has_cgst_sgst     = true;
        break;
    case SupplyType::INTER:
        b.igst_cents        = money::taxOn(base_cents, tax_rate_bps);
        b.has_igst          = true;
        break;
    }
    b.total_tax_cents = b.cgst_cents + b.sgst_cents + b.igst_cents + b.cess_cents;
    return b;
}

StateMatrix StateMatrix::validateAndSanitize(const json& doc)
{
    StateMatrix m;

    // fail(path, why) terminates the build with a precise diagnostic.
    const auto fail = [](const std::string& path, const std::string& why) {
        throw std::invalid_argument("StateMatrix::validateAndSanitize: " + path + ": " + why);
    };

    if (!doc.is_object())
    {
        fail("document", "root must be a JSON object");
    }

    // ---- supplier ----------------------------------------------------------
    if (!doc.contains("supplier") || !doc["supplier"].is_object())
    {
        fail("supplier", "required object not present");
    }
    const json& sup = doc["supplier"];
    m.supplier_gstin_  = requireString(sup.at("gstin"), "supplier.gstin");
    m.supplier_name_   = requireString(sup.at("name"), "supplier.name");
    m.supplier_state_  = requireString(sup.at("address_state"), "supplier.address_state");
    if (sup.contains("aato_crore"))
    {
        if (!sup["aato_crore"].is_number())
        {
            fail("supplier.aato_crore", "must be a number");
        }
        const double aato = sup["aato_crore"].get<double>();
        if (aato < 0.0 || !std::isfinite(aato))
        {
            fail("supplier.aato_crore", "must be finite and non-negative");
        }
        m.aato_crore_ = aato;
    }

    // ---- buyer (optional) ---------------------------------------------------
    if (doc.contains("buyer"))
    {
        const json& by = doc["buyer"];
        if (!by.is_object())
        {
            fail("buyer", "must be an object");
        }
        if (by.contains("registered"))
        {
            if (!by["registered"].is_boolean())
            {
                fail("buyer.registered", "must be a boolean");
            }
            m.buyer_registered_ = by["registered"].get<bool>();
        }
        if (by.contains("gstin"))
        {
            m.buyer_gstin_ = requireString(by.at("gstin"), "buyer.gstin");
        }
        if (by.contains("name"))
        {
            m.buyer_name_ = requireString(by.at("name"), "buyer.name");
        }
        if (by.contains("address_state"))
        {
            m.buyer_state_ = requireString(by.at("address_state"), "buyer.address_state");
        }
    }

    // ---- invoice -------------------------------------------------------------
    if (!doc.contains("invoice") || !doc["invoice"].is_object())
    {
        fail("invoice", "required object not present");
    }
    const json& inv = doc["invoice"];
    m.invoice_number_     = requireString(inv.at("number"), "invoice.number");
    m.invoice_date_       = requireString(inv.at("date"), "invoice.date");
    m.place_of_supply_    = requireString(inv.at("place_of_supply"), "invoice.place_of_supply");
    m.hsn_string_         = requireString(inv.at("hsn_string"), "invoice.hsn_string");

    const std::string& st = requireString(inv.at("supply_type"), "invoice.supply_type");
    if (st == "INTRA")
    {
        m.supply_type_ = SupplyType::INTRA;
    }
    else if (st == "INTER")
    {
        m.supply_type_ = SupplyType::INTER;
    }
    else
    {
        fail("invoice.supply_type", "must be 'INTRA' or 'INTER'");
    }

    const std::string& cls = requireString(inv.at("classification"), "invoice.classification");
    if (cls == "B2B")
    {
        m.classification_ = Classification::B2B;
    }
    else if (cls == "B2C")
    {
        m.classification_ = Classification::B2C;
    }
    else if (cls == "UNDETERMINED")
    {
        m.classification_ = Classification::UNDETERMINED;
    }
    else
    {
        fail("invoice.classification", "must be 'B2B', 'B2C' or 'UNDETERMINED'");
    }

    if (!inv.contains("irn_present") || !inv["irn_present"].is_boolean())
    {
        fail("invoice.irn_present", "required boolean not present");
    }
    m.irn_present_ = inv["irn_present"].get<bool>();
    if (m.irn_present_)
    {
        if (!inv.contains("irn") || !inv["irn"].is_string())
        {
            fail("invoice.irn", "required string when irn_present is true");
        }
        m.irn_ = inv["irn"].get<std::string>();
    }

    if (!inv.contains("base_value"))
    {
        fail("invoice.base_value", "required number not present");
    }
    m.base_value_cents_ = money::toCents(inv["base_value"]);

    // ---- line items -----------------------------------------------------------
    if (!inv.contains("line_items") || !inv["line_items"].is_array())
    {
        fail("invoice.line_items", "required array not present");
    }
    const json& items = inv["line_items"];
    m.line_items_.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        const std::string liPath = "invoice.line_items[" + std::to_string(i) + "]";
        const json& li = items[i];
        if (!li.is_object())
        {
            fail(liPath, "line item must be an object");
        }
        LineItem item;
        item.hsn            = requireString(li.at("hsn"), liPath + ".hsn");
        item.tax_rate_bps   = rateToBps(li.at("tax_rate"), liPath + ".tax_rate");
        item.base_value_cents = money::toCents(li.at("base_value"));
        if (li.contains("description"))
        {
            item.description = requireString(li.at("description"), liPath + ".description");
        }
        if (li.contains("quantity"))
        {
            if (!li["quantity"].is_number())
            {
                fail(liPath + ".quantity", "must be a number");
            }
            item.quantity = li["quantity"].get<double>();
        }
        if (li.contains("unit_price"))
        {
            item.unit_price_cents = money::toCents(li["unit_price"]);
        }
        m.line_items_.push_back(std::move(item));
    }

    // ---- tax breakdown ----------------------------------------------------------
    if (!inv.contains("tax_breakdown") || !inv["tax_breakdown"].is_object())
    {
        fail("invoice.tax_breakdown", "required object not present");
    }
    const json& tb = inv["tax_breakdown"];
    if (tb.contains("cgst")) m.tax_.cgst_cents = money::toCents(tb["cgst"]);
    if (tb.contains("sgst")) m.tax_.sgst_cents = money::toCents(tb["sgst"]);
    if (tb.contains("igst")) m.tax_.igst_cents = money::toCents(tb["igst"]);
    if (tb.contains("cess")) m.tax_.cess_cents = money::toCents(tb["cess"]);
    if (tb.contains("total_tax"))
    {
        m.tax_.total_tax_cents = money::toCents(tb["total_tax"]);
    }
    else
    {
        m.tax_.total_tax_cents = m.tax_.cgst_cents + m.tax_.sgst_cents +
                                 m.tax_.igst_cents + m.tax_.cess_cents;
    }
    // Truth dwells in the amounts: the per-head flags are derived, not trusted
    // from the payload, so a contradiction in the booleans cannot propagate.
    m.tax_.has_cgst_sgst = (m.tax_.cgst_cents > 0 || m.tax_.sgst_cents > 0);
    m.tax_.has_igst      = (m.tax_.igst_cents > 0);

    if (!inv.contains("total_value"))
    {
        fail("invoice.total_value", "required number not present");
    }
    m.total_value_cents_ = money::toCents(inv["total_value"]);

    // ---- IRP metadata (optional) ------------------------------------------------
    if (doc.contains("irp"))
    {
        const json& irp = doc["irp"];
        if (!irp.is_object())
        {
            fail("irp", "must be an object");
        }
        if (irp.contains("reported_on"))
        {
            m.irp_reported_on_ = requireString(irp.at("reported_on"), "irp.reported_on");
        }
    }

    return m;
}

bool StateMatrix::validateAgainstSchema(const json& doc, const json& schema,
                                        std::vector<std::string>& out_errors)
{
    out_errors.clear();
    if (!schema.is_object())
    {
        out_errors.emplace_back("schema: root must be an object");
        return false;
    }
    if (!doc.is_object())
    {
        out_errors.emplace_back("document: root must be an object");
        return false;
    }

    // Fatal-vs-report: this walk never throws; it reports every discrepancy.
    struct Walker
    {
        static void node(const json& actual, const json& schema,
                         const std::string& path, std::vector<std::string>& errs)
        {
            // A schema node with no "$type" is a BARE object spec — e.g. the
            // gemini_schema "line_items.$item" container. Default it to object
            // so per-field specs inside are walked, not rejected as unknown.
            std::string type = schema.value("$type", "");
            if (type.empty())
            {
                type = "object";
            }
            if (type == "object")
            {
                if (!actual.is_object())
                {
                    errs.push_back(path + ": expected object");
                    return;
                }
                for (auto it = schema.begin(); it != schema.end(); ++it)
                {
                    const std::string& key = it.key();
                    if (key.empty() || key.front() == '$') continue;   // metadata / $item
                    if (!it.value().contains("$type")) continue;       // description etc.
                    const bool required = it.value().value("$required", false);
                    if (!actual.contains(key))
                    {
                        if (required) errs.push_back(path + "." + key + ": missing required field");
                        continue;
                    }
                    node(actual[key], it.value(), path + "." + key, errs);
                }
            }
            else if (type == "array")
            {
                if (!actual.is_array())
                {
                    errs.push_back(path + ": expected array");
                    return;
                }
                if (schema.contains("$item"))
                {
                    const json& itemSchema = schema["$item"];
                    for (std::size_t i = 0; i < actual.size(); ++i)
                    {
                        node(actual[i], itemSchema,
                             path + "[" + std::to_string(i) + "]", errs);
                    }
                }
            }
            else if (type == "string")
            {
                if (!actual.is_string()) errs.push_back(path + ": expected string");
            }
            else if (type == "number")
            {
                if (!actual.is_number()) errs.push_back(path + ": expected number");
            }
            else if (type == "boolean")
            {
                if (!actual.is_boolean()) errs.push_back(path + ": expected boolean");
            }
            else
            {
                errs.push_back(path + ": unknown schema $type '" + type + "'");
            }
        }
    };

    for (auto it = schema.begin(); it != schema.end(); ++it)
    {
        const std::string& key = it.key();
        if (key.empty() || key.front() == '$') continue;
        if (!it.value().contains("$type")) continue;
        const bool required = it.value().value("$required", false);
        if (!doc.contains(key))
        {
            if (required) out_errors.push_back("document: missing required field '" + key + "'");
            continue;
        }
        Walker::node(doc[key], it.value(), key, out_errors);
    }

    return out_errors.empty();
}

} // namespace tax
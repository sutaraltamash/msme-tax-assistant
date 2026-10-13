// gstrgenerator_test.cpp — unit tests for the deterministic GST return CSVs.
//
// Covers: header/row shape for GSTR-1 Table 4A, B2C exclusion, intra-vs-inter
// leg placement, HSN summary aggregation and rate grouping, GSTR-3B bucket
// omission, CSV escaping, fixed 2-dp rupee formatting, recompute-over-claims,
// and empty-input behavior.

#include <sstream>
#include <string>
#include <vector>

#include "GSTRGenerator.hpp"
#include "StateMatrix.hpp"
#include "fixtures.h"
#include "test_framework.h"

using namespace tax;
using namespace testfix;
using json = nlohmann::json;

namespace {

// Split CSV body into data rows (expects CRLF terminators; drops the trailing
// empty token). Returns non-header lines.
std::vector<std::string> dataRows(const std::string& csv, std::string& header)
{
    std::vector<std::string> rows;
    std::size_t start = 0;
    while (true)
    {
        const std::size_t crlf = csv.find("\r\n", start);
        if (crlf == std::string::npos)
        {
            if (start < csv.size()) rows.push_back(csv.substr(start));
            break;
        }
        rows.push_back(csv.substr(start, crlf - start));
        start = crlf + 2;
    }
    if (!rows.empty()) { header = rows.front(); rows.erase(rows.begin()); }
    return rows;
}

bool contains(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}

// A B2B intra-state invoice with an extra 5% line (for rate grouping).
json mixedRateIntraDoc()
{
    auto doc = intraDoc();
    doc["invoice"]["line_items"].push_back(
        json{{"hsn", "84713000"},
             {"description", "Warranty"},
             {"quantity", 1},
             {"base_value", 400.00},
             {"tax_rate", 5}});
    return doc;
}

} // anonymous namespace

// ---- GSTR-1 Table 4A (B2B) ---------------------------------------------------

TEST(gstr1_b2b_writes_header_and_intra_row)
{
    const auto m = StateMatrix::validateAndSanitize(intraDoc());
    std::string header;
    const auto rows = dataRows(GSTRGenerator::gstr1B2B({m}), header);

    CHECK(header == "gstin_recipient,invoice_number,invoice_date,invoice_value,"
                    "place_of_supply,supply_type,gst_rate,taxable_value,igst_amt,"
                    "cgst_amt,sgst_amt,cess_amt");
    CHECK_EQ(rows.size(), 1u);
    CHECK(contains(rows[0], "27FGHIJ5678K1Z3"));      // recipient GSTIN
    CHECK(contains(rows[0], "INV-2026-0002"));        // invoice number
    CHECK(contains(rows[0], "2026-04-02"));           // invoice date
    CHECK(contains(rows[0], ",MH,"));                 // place of supply (intra)
    CHECK(contains(rows[0], ",Intra,"));
    CHECK(contains(rows[0], ",18,"));                 // rate percent
    CHECK(contains(rows[0], ",1000.00,"));            // taxable value
    CHECK(contains(rows[0], ",0.00,"));               // igst on intra
    CHECK(contains(rows[0], "90.00,90.00,0.00"));     // cgst, sgst, cess
    CHECK(contains(rows[0], "1180.00"));              // recomputed invoice value
}

TEST(gstr1_b2b_inter_invoice_places_igst)
{
    const auto m = StateMatrix::validateAndSanitize(interB2BDoc());
    std::string header;
    const auto rows = dataRows(GSTRGenerator::gstr1B2B({m}), header);

    CHECK_EQ(rows.size(), 1u);
    CHECK(contains(rows[0], ",KA,"));                 // place of supply
    CHECK(contains(rows[0], ",Inter,"));
    CHECK(contains(rows[0], ",1000.00,180.00,0.00,0.00,0.00"));  // igst placed, cgst/sgst zero
    CHECK(contains(rows[0], "1180.00"));
}

TEST(gstr1_b2b_excludes_b2c_invoices)
{
    const auto inter = StateMatrix::validateAndSanitize(interB2BDoc());
    const auto b2c   = StateMatrix::validateAndSanitize(b2cDoc());
    std::string header;
    const auto rows = dataRows(GSTRGenerator::gstr1B2B({inter, b2c}), header);

    CHECK_EQ(rows.size(), 1u);                        // only the B2B invoice
    CHECK(contains(rows[0], "INV-2026-0001"));
    CHECK(!contains(GSTRGenerator::gstr1B2B({inter, b2c}), "INV-2026-0003"));
}

TEST(gstr1_b2b_recomputes_tax_not_claims)
{
    // The extraction's claimed totals are wrong (INR 100 tax), but the CSV
    // must report the RECOMPUTED statutory figures (180.00) from the lines.
    auto doc = intraDoc();
    doc["invoice"]["tax_breakdown"]["cgst"]       = 50.0;
    doc["invoice"]["tax_breakdown"]["sgst"]       = 50.0;
    doc["invoice"]["tax_breakdown"]["total_tax"]  = 100.0;
    doc["invoice"]["total_value"]                 = 1100.0;
    const auto m = StateMatrix::validateAndSanitize(doc);

    const std::string csv = GSTRGenerator::gstr1B2B({m});
    CHECK(!contains(csv, "1100.00"));
    CHECK(contains(csv, "1180.00"));               // 1000.00 taxable + 180.00 tax
    CHECK(contains(csv, "90.00,90.00"));
}

// ---- GSTR-1 HSN summary ------------------------------------------------------

TEST(hsn_summary_aggregates_all_invoices)
{
    const auto intra = StateMatrix::validateAndSanitize(intraDoc());   // qty 1, 18%
    const auto inter = StateMatrix::validateAndSanitize(interB2BDoc()); // qty 2, 18%
    std::string header;
    const auto rows = dataRows(GSTRGenerator::gstr1HsnSummary({intra, inter}), header);

    CHECK_EQ(rows.size(), 1u);                     // same (hsn, rate) across both
    CHECK(contains(rows[0], "84713000,18"));
    CHECK(contains(rows[0], "2000.00"));           // 1000.00 + 1000.00
    // igst 180.00 (inter-only), cgst/sgst 90.00 each (intra-only), cess 0, qty 3.
    CHECK(contains(rows[0], ",2000.00,180.00,90.00,90.00,0.00,3.00"));
    CHECK(contains(rows[0], "3.00"));              // quantities 1 + 2
}

TEST(hsn_summary_groups_by_rate)
{
    const auto m = StateMatrix::validateAndSanitize(mixedRateIntraDoc());  // 18% + 5%
    std::string header;
    const auto rows = dataRows(GSTRGenerator::gstr1HsnSummary({m}), header);

    CHECK_EQ(rows.size(), 2u);                     // two rates, same HSN
    CHECK(contains(rows[0], ",5,"));
    CHECK(contains(rows[0], "400.00"));
    CHECK(contains(rows[1], ",18,"));
    CHECK(contains(rows[1], "1000.00"));
}

// ---- GSTR-3B Table 4 ----------------------------------------------------------

TEST(gstr3b_buckets_by_supply_type)
{
    const auto intra = StateMatrix::validateAndSanitize(intraDoc());
    const auto inter = StateMatrix::validateAndSanitize(interB2BDoc());
    std::string header;
    const auto rows = dataRows(GSTRGenerator::gstr3B({intra, inter}), header);

    CHECK_EQ(rows.size(), 2u);
    CHECK(contains(rows[0], "4A-Intra"));
    CHECK(contains(rows[0], "1000.00"));
    CHECK(contains(rows[0], "90.00,90.00,0.00,180.00"));    // cgst,sgst,cess,total
    CHECK(contains(rows[1], "4A-Inter"));
    CHECK(contains(rows[1], ",1000.00,180.00,0.00,0.00,0.00,180.00"));
}

TEST(gstr3b_omits_absent_bucket)
{
    const auto intra = StateMatrix::validateAndSanitize(intraDoc());
    const std::string csv = GSTRGenerator::gstr3B({intra});

    CHECK(contains(csv, "4A-Intra"));
    CHECK(!contains(csv, "4A-Inter"));             // no inter-state supply filed
}

// ---- CSV mechanics ------------------------------------------------------------

TEST(csv_escapes_special_characters)
{
    auto doc = interB2BDoc();
    doc["invoice"]["number"] = "INV,\"Q1\"-001";   // comma + quotes in a field
    const auto m = StateMatrix::validateAndSanitize(doc);

    const std::string csv = GSTRGenerator::gstr1B2B({m});
    // RFC-4180 doubling: the raw field appears quoted with inner quotes doubled.
    CHECK(contains(csv, "\"INV,\"\"Q1\"\"-001\""));
}

TEST(money_columns_are_fixed_two_decimals)
{
    const auto m = StateMatrix::validateAndSanitize(intraDoc());
    const std::string csv = GSTRGenerator::gstr3B({m});
    CHECK(contains(csv, "1000.00"));
    CHECK(contains(csv, "90.00"));
    CHECK(contains(csv, "180.00"));
    CHECK(!contains(csv, ",1000,"));               // "1000.00" present, never bare 1000
}

TEST(empty_input_yields_header_only)
{
    std::string header;
    const auto rows = dataRows(GSTRGenerator::gstr3B({}), header);
    CHECK(header == "section,description,taxable_value,igst_amt,cgst_amt,"
                    "sgst_amt,cess_amt,total_tax_amt");
    CHECK(rows.empty());

    std::string h2;
    const auto r2 = dataRows(GSTRGenerator::gstr1B2B({}), h2);
    CHECK(!h2.empty());
    CHECK(r2.empty());
}

TESTS_BEGIN
RUN_TEST(gstr1_b2b_writes_header_and_intra_row),
RUN_TEST(gstr1_b2b_inter_invoice_places_igst),
RUN_TEST(gstr1_b2b_excludes_b2c_invoices),
RUN_TEST(gstr1_b2b_recomputes_tax_not_claims),
RUN_TEST(hsn_summary_aggregates_all_invoices),
RUN_TEST(hsn_summary_groups_by_rate),
RUN_TEST(gstr3b_buckets_by_supply_type),
RUN_TEST(gstr3b_omits_absent_bucket),
RUN_TEST(csv_escapes_special_characters),
RUN_TEST(money_columns_are_fixed_two_decimals),
RUN_TEST(empty_input_yields_header_only),
TESTS_END
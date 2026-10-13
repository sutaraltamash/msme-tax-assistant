// repository_test.cpp — unit tests for the SQLite persistence layer.
//
// Covers: session lifecycle (create/exists/status), integer-CENTS money on
// disk (verified against a raw second sqlite3 handle — the "0% hallucination"
// money guarantee must hold at rest, not just in memory), lossless StateMatrix
// rehydration from document_json, ingestion-order preservation, the chat
// transcript, and fail-fast rejection of corrupt persisted documents.

#include <cstdio>
#include <string>
#include <vector>

#include "InvoiceRepository.hpp"
#include "StateMatrix.hpp"
#include "fixtures.h"
#include "test_framework.h"

#include "sqlite3.h"

using namespace tax;
using namespace testfix;

namespace {

// Unique throwaway DB path per test; removed on destruction (also -wal/-shm).
struct TempDb
{
    std::string path;

    TempDb()
    {
        static int counter = 0;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "repo_test_%03d.db", ++counter);
        path = buf;
    }
    ~TempDb()
    {
        std::remove(path.c_str());
        std::remove((path + "-wal").c_str());
        std::remove((path + "-shm").c_str());
    }
    const std::string& str() const { return path; }
};

bool has(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}

} // anonymous namespace

// ---- session lifecycle -----------------------------------------------------

TEST(create_session_exists_with_active_status)
{
    TempDb db;
    InvoiceRepository repo(db.str());
    repo.createSession("s-one");

    CHECK(repo.sessionExists("s-one"));
    CHECK(!repo.sessionExists("nope"));
    CHECK(repo.sessionStatus("s-one") == "ACTIVE");
    CHECK_EQ(repo.sessionCount(), 1);
}

TEST(set_session_status_roundtrips_and_unknown_is_quiet_noop)
{
    TempDb db;
    InvoiceRepository repo(db.str());
    repo.createSession("s-two");

    repo.setSessionStatus("s-two", "INTERROGATING");
    CHECK(repo.sessionStatus("s-two") == "INTERROGATING");
    repo.setSessionStatus("s-two", "FILED");
    CHECK(repo.sessionStatus("s-two") == "FILED");

    repo.setSessionStatus("does-not-exist", "FILED");   // no throw, no row
    CHECK_EQ(repo.sessionCount(), 1);
}

// ---- persistence round-trip -------------------------------------------------

TEST(save_extraction_roundtrips_matrix_values)
{
    TempDb db;
    InvoiceRepository repo(db.str());
    repo.createSession("s-rt");

    const auto doc   = interB2BDoc();
    const auto matrix = StateMatrix::validateAndSanitize(doc);
    const auto id = repo.saveExtraction("s-rt", "scan.jpg", "image/jpeg", matrix, doc, {"low contrast"});

    CHECK(id > 0);
    auto restored = repo.invoicesForSession("s-rt");
    CHECK_EQ(restored.size(), 1u);

    const StateMatrix& m = restored[0];
    CHECK(m.invoiceNumber() == "INV-2026-0001");
    CHECK_EQ(m.baseValueCents(), 100000);
    CHECK_EQ(m.totalTaxCents(), 18000);
    CHECK_EQ(m.totalValueCents(), 118000);
    CHECK(m.supplyType() == SupplyType::INTER);
    CHECK(m.classification() == Classification::B2B);
    CHECK(m.hsnString() == "84713000");
    CHECK(m.buyerGstin().has_value());
    CHECK(*m.buyerGstin() == "29ABCDE1234F1Z5");
    CHECK(m.tax().has_igst);
    CHECK(!m.tax().has_cgst_sgst);
    CHECK_EQ(m.lineItems().size(), 1u);
    CHECK(m.lineItems()[0].hsn == "84713000");
    CHECK_EQ(m.lineItems()[0].tax_rate_bps, 1800);
}

TEST(money_is_persisted_as_integer_cents)
{
    TempDb db;
    {
        InvoiceRepository repo(db.str());
        repo.createSession("s-money");
        const auto doc    = interB2BDoc();
        const auto matrix = StateMatrix::validateAndSanitize(doc);
        repo.saveExtraction("s-money", "", "image/jpeg", matrix, doc, {});
    }

    // Second, raw handle: assert on the COLUMNS as written, not on a
    // re-validation of document_json — proves integer cents at rest.
    sqlite3* raw = nullptr;
    CHECK(sqlite3_open(db.str().c_str(), &raw) == SQLITE_OK);

    auto q0 = [&](const char* sql, std::int64_t& out) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(raw, sql, -1, &st, nullptr) != SQLITE_OK) return false;
        const bool ok = sqlite3_step(st) == SQLITE_ROW;
        if (ok) out = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        return ok;
    };

    std::int64_t base = 0, tax = 0, total = 0, irn = 0;
    CHECK(q0("SELECT base_value_cents  FROM invoices", base));
    CHECK(q0("SELECT total_tax_cents   FROM invoices", tax));
    CHECK(q0("SELECT total_value_cents FROM invoices", total));
    CHECK(q0("SELECT irn_present        FROM invoices", irn));
    CHECK_EQ(base, 100000);
    CHECK_EQ(tax, 18000);
    CHECK_EQ(total, 118000);
    CHECK_EQ(irn, 1);

    sqlite3_stmt* li = nullptr;
    CHECK(sqlite3_prepare_v2(raw,
        "SELECT quantity, unit_price_cents, base_value_cents, tax_rate_bps "
        "FROM line_items", -1, &li, nullptr) == SQLITE_OK);
    CHECK(sqlite3_step(li) == SQLITE_ROW);
    CHECK_EQ(sqlite3_column_double(li, 0), 2.0);
    CHECK_EQ(sqlite3_column_int64(li, 1), 50000);
    CHECK_EQ(sqlite3_column_int64(li, 2), 100000);
    CHECK_EQ(sqlite3_column_int64(li, 3), 1800);
    sqlite3_finalize(li);

    CHECK(sqlite3_close(raw) == SQLITE_OK);
}

TEST(multiple_extractions_preserve_ingestion_order)
{
    TempDb db;
    InvoiceRepository repo(db.str());
    repo.createSession("s-order");

    auto inter = StateMatrix::validateAndSanitize(interB2BDoc());
    auto intra = StateMatrix::validateAndSanitize(intraDoc());
    repo.saveExtraction("s-order", "", "image/jpeg", inter, interB2BDoc(), {});
    repo.saveExtraction("s-order", "", "image/jpeg", intra, intraDoc(), {});

    auto restored = repo.invoicesForSession("s-order");
    CHECK_EQ(restored.size(), 2u);
    CHECK(restored[0].invoiceNumber() == "INV-2026-0001");
    CHECK(restored[1].invoiceNumber() == "INV-2026-0002");
}

// ---- corrupt persisted document is surfaced, never silently filed -------------

TEST(corrupt_persisted_document_fails_rehydration)
{
    TempDb db;
    {
        InvoiceRepository repo(db.str());
        repo.createSession("s-bad");

        // Write a row whose document_json is not even valid JSON.
        sqlite3* raw = nullptr;
        CHECK(sqlite3_open(db.str().c_str(), &raw) == SQLITE_OK);
        sqlite3_stmt* st = nullptr;
        const char* ins =
            "INSERT INTO invoices (session_id, invoice_number, invoice_date, "
            "classification, supply_type, place_of_supply, base_value_cents, "
            "total_tax_cents, total_value_cents, document_json) "
            "VALUES ('s-bad','X','2026-01-01','B2B','INTRA','MH',0,0,0,'{bad')";
        CHECK(sqlite3_prepare_v2(raw, ins, -1, &st, nullptr) == SQLITE_OK);
        CHECK(sqlite3_step(st) == SQLITE_DONE);
        sqlite3_finalize(st);
        CHECK(sqlite3_close(raw) == SQLITE_OK);
    }

    InvoiceRepository repo(db.str());
    CHECK_THROWS(repo.invoicesForSession("s-bad"), std::runtime_error);
}

// ---- chat transcript ---------------------------------------------------------

TEST(chat_messages_store_and_reload_in_order)
{
    TempDb db;
    InvoiceRepository repo(db.str());
    repo.createSession("s-chat");

    repo.addChatMessage("s-chat", "assistant",
                        "IRP reporting timestamp missing.", "INPUT_TEXT");
    repo.addChatMessage("s-chat", "user", "Reported on 2026-04-10", "");
    repo.addChatMessage("s-chat", "assistant", "Recorded.", "");

    const auto history = repo.chatHistory("s-chat");
    CHECK_EQ(history.size(), 3u);
    CHECK(history[0].role == "assistant");
    CHECK(history[0].action_required == "INPUT_TEXT");
    CHECK(history[1].role == "user");
    CHECK(history[1].content == "Reported on 2026-04-10");
    CHECK(history[2].role == "assistant");
    CHECK(has(history[0].created_at, "T"));   // ISO-8601 timestamp populated
}

// ---- diagnostics ------------------------------------------------------------

TEST(unknown_session_filing_target_has_no_invoices)
{
    TempDb db;
    InvoiceRepository repo(db.str());
    CHECK(repo.invoicesForSession("never-created").empty());
    CHECK(repo.chatHistory("never-created").empty());
}

TESTS_BEGIN
RUN_TEST(create_session_exists_with_active_status),
RUN_TEST(set_session_status_roundtrips_and_unknown_is_quiet_noop),
RUN_TEST(save_extraction_roundtrips_matrix_values),
RUN_TEST(money_is_persisted_as_integer_cents),
RUN_TEST(multiple_extractions_preserve_ingestion_order),
RUN_TEST(corrupt_persisted_document_fails_rehydration),
RUN_TEST(chat_messages_store_and_reload_in_order),
RUN_TEST(unknown_session_filing_target_has_no_invoices),
TESTS_END
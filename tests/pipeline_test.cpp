// pipeline_test.cpp — end-to-end orchestration seam tests.
//
// Drives Pipeline (MockExtractor + RuleEngine + InvoiceRepository +
// GSTRGenerator) exactly as the WebSocket layer does, asserting on the
// protocol frames. Covers: session creation, gap-free upload vs. gap
// interrogation, USER_REPLY archival, FILING_READY CSV delivery, malformed
// extraction rejection, cold-start filing from the DB (restart), and the
// file-name/frame shape of the deliverable.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "MockExtractor.hpp"
#include "Pipeline.hpp"
#include "fixtures.h"
#include "test_framework.h"

using namespace tax;
using namespace testfix;
using json = nlohmann::json;

namespace {

// Compile-time rule config path injected by tests/CMakeLists.txt.
const std::string kRulesPath = RULES_CONFIG_PATH;

struct TempDb
{
    std::string path;

    TempDb()
    {
        static int counter = 0;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "pipe_test_%03d.db", ++counter);
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

int countType(const PipelineResult& r, const std::string& type)
{
    int n = 0;
    for (const auto& f : r.frames) if (f.value("type", "") == type) ++n;
    return n;
}

bool assistantSays(const PipelineResult& r, const std::string& needle)
{
    for (const auto& f : r.frames)
    {
        if (f.value("type", "") == "CHAT_MESSAGE" &&
            f.value("role", "") == "assistant" &&
            std::string(f.value("content", "")).find(needle) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

bool hasInputGapFrame(const PipelineResult& r, const std::string& needle = "")
{
    for (const auto& f : r.frames)
    {
        if (f.value("type", "") != "CHAT_MESSAGE") continue;
        if (!f.contains("action_required") || !f["action_required"].is_string()) continue;
        if (needle.empty()) return true;
        if (std::string(f.value("content", "")).find(needle) != std::string::npos) return true;
    }
    return false;
}

const json* findType(const PipelineResult& r, const std::string& type)
{
    for (const auto& f : r.frames) if (f.value("type", "") == type) return &f;
    return nullptr;
}

} // anonymous namespace

// ---- sessions ---------------------------------------------------------------

TEST(open_session_creates_distinct_persisted_sessions)
{
    TempDb db;
    Pipeline pipeline(std::make_unique<MockExtractor>(), kRulesPath, db.str());

    const std::string a = pipeline.openSession();
    const std::string b = pipeline.openSession();

    CHECK(!a.empty());
    CHECK(a != b);
    CHECK(pipeline.repository().sessionExists(a));
    CHECK(pipeline.repository().sessionExists(b));
    CHECK_EQ(pipeline.repository().sessionCount(), 2);
}

// ---- UPLOAD_INVOICE ---------------------------------------------------------

TEST(upload_compliant_document_passes_clean)
{
    TempDb db;
    // Default MockExtractor is fully compliant: 0 gaps.
    Pipeline pipeline(std::make_unique<MockExtractor>(), kRulesPath, db.str());
    const std::string sid = pipeline.openSession();

    const auto result = pipeline.processUpload(sid, "aGVsbG8=", "image/jpeg");

    CHECK(result.session_status == "ACTIVE");
    CHECK(assistantSays(result, "Verification complete"));
    CHECK(!hasInputGapFrame(result));
    CHECK(!assistantSays(result, "open compliance gap"));
    CHECK_EQ(countType(result, "FILING_READY"), 0);
    CHECK_EQ(pipeline.repository().invoicesForSession(sid).size(), 1u);
}

TEST(upload_gap_document_interrogates_with_rule_message)
{
    TempDb db;
    // intraDoc: e-invoiced but no IRP timestamp -> R-IRP-002 violation.
    Pipeline pipeline(std::make_unique<MockExtractor>(intraDoc()), kRulesPath, db.str());
    const std::string sid = pipeline.openSession();

    const auto result = pipeline.processUpload(sid, "aGVsbG8=", "application/pdf");

    CHECK(result.session_status == "INTERROGATING");
    CHECK(hasInputGapFrame(result, "30 days"));            // R-IRP-002 message text
    CHECK(assistantSays(result, "1 open compliance gap"));
    CHECK_EQ(pipeline.repository().invoicesForSession(sid).size(), 1u);
}

TEST(upload_malformed_extraction_is_rejected_before_persist)
{
    TempDb db;
    // Extraction that violates the gemini_schema contract -> rejected at the
    // StateMatrix gate; nothing may reach the DB.
    Pipeline pipeline(std::make_unique<MockExtractor>(json{{"foo", "bar"}}),
                      kRulesPath, db.str());
    const std::string sid = pipeline.openSession();

    const auto result = pipeline.processUpload(sid, "aGVsbG8=", "image/png");

    CHECK(assistantSays(result, "did not pass the schema contract"));
    CHECK_EQ(countType(result, "FILING_READY"), 0);
    CHECK(pipeline.repository().invoicesForSession(sid).empty());
}

TEST(upload_unknown_session_is_rejected)
{
    TempDb db;
    Pipeline pipeline(std::make_unique<MockExtractor>(), kRulesPath, db.str());

    const auto result = pipeline.processUpload("ghost", "aGVsbG8=", "image/jpeg");

    CHECK(assistantSays(result, "Unknown session"));
}

// ---- USER_REPLY -------------------------------------------------------------

TEST(user_reply_is_archived_and_acknowledged)
{
    TempDb db;
    Pipeline pipeline(std::make_unique<MockExtractor>(intraDoc()), kRulesPath, db.str());
    const std::string sid = pipeline.openSession();
    pipeline.processUpload(sid, "aGVsbG8=", "image/jpeg");

    const auto result = pipeline.processUserReply(sid, "The IRP was reported on 2026-04-10.");

    CHECK(assistantSays(result, "recorded"));
    CHECK(result.session_status == "INTERROGATING");       // still open after reply

    const auto history = pipeline.repository().chatHistory(sid);
    bool userArchived = false;
    for (const auto& m : history)
    {
        if (m.role == "user") userArchived = true;
    }
    CHECK(userArchived);
}

// ---- REQUEST_FILING ---------------------------------------------------------

TEST(request_filing_emits_three_csv_files)
{
    TempDb db;
    Pipeline pipeline(std::make_unique<MockExtractor>(), kRulesPath, db.str());
    const std::string sid = pipeline.openSession();
    pipeline.processUpload(sid, "aGVsbG8=", "image/jpeg");

    const auto result = pipeline.requestFiling(sid);

    const json* ready = findType(result, "FILING_READY");
    CHECK(ready != nullptr);
    if (ready != nullptr)
    {
        const json& files = (*ready)["files"];
        CHECK(files.contains("gstr1_b2b.csv"));
        CHECK(files.contains("gstr1_hsn_summary.csv"));
        CHECK(files.contains("gstr3b_summary.csv"));

        const std::string g1  = files["gstr1_b2b.csv"].get<std::string>();
        const std::string hsn = files["gstr1_hsn_summary.csv"].get<std::string>();
        const std::string g3  = files["gstr3b_summary.csv"].get<std::string>();

        CHECK(g1.find("gstin_recipient") != std::string::npos);
        CHECK(g1.find("INV-2026-0001") != std::string::npos);   // B2B rows carry no HSN column
        CHECK(g1.find("1000.00") != std::string::npos);
        CHECK(hsn.find("hsn_code,gst_rate") != std::string::npos);
        CHECK(hsn.find("84713000,18") != std::string::npos);
        CHECK(g3.find("4A-Inter") != std::string::npos);
        CHECK(g3.find(",1000.00,180.00,0.00,0.00,0.00,180.00") != std::string::npos);
    }
    CHECK(pipeline.repository().sessionStatus(sid) == "FILED");
    CHECK(pipeline.statusOf(sid) == SessionStatus::FILED);
    CHECK(result.session_status == "FILED");
}

TEST(filing_after_restart_rehydrates_from_database)
{
    TempDb db;
    std::string sid;
    {
        Pipeline first(std::make_unique<MockExtractor>(), kRulesPath, db.str());
        sid = first.openSession();
        first.processUpload(sid, "aGVsbG8=", "image/jpeg");
    }   // "server" goes away; only the SQLite file survives

    Pipeline second(std::make_unique<MockExtractor>(), kRulesPath, db.str());
    const auto result = second.requestFiling(sid);       // cold-start session id

    const json* ready = findType(result, "FILING_READY");
    CHECK(ready != nullptr);
    if (ready != nullptr)
    {
        const json& files = (*ready)["files"];
        CHECK(files.contains("gstr1_b2b.csv"));
        CHECK(std::string(files["gstr1_b2b.csv"].get<std::string>())
                  .find("INV-2026-0001") != std::string::npos);
    }
}

TEST(request_filing_with_no_invoices_is_an_error_not_a_file)
{
    TempDb db;
    Pipeline pipeline(std::make_unique<MockExtractor>(), kRulesPath, db.str());
    const std::string sid = pipeline.openSession();

    const auto result = pipeline.requestFiling(sid);

    CHECK_EQ(countType(result, "FILING_READY"), 0);
    CHECK(assistantSays(result, "No verified invoices"));
    CHECK(pipeline.repository().sessionStatus(sid) != "FILED");
}

// ---- session lifecycle ------------------------------------------------------

TEST(close_session_abandons_unless_filed)
{
    TempDb db;
    Pipeline pipeline(std::make_unique<MockExtractor>(), kRulesPath, db.str());

    const std::string unfiled = pipeline.openSession();
    pipeline.closeSession(unfiled);
    CHECK(pipeline.statusOf(unfiled) == SessionStatus::ABANDONED);

    const std::string filed = pipeline.openSession();
    pipeline.processUpload(filed, "aGVsbG8=", "image/jpeg");
    pipeline.requestFiling(filed);
    pipeline.closeSession(filed);          // FILED sessions stay FILED
    CHECK(pipeline.statusOf(filed) == SessionStatus::FILED);
}

TESTS_BEGIN
RUN_TEST(open_session_creates_distinct_persisted_sessions),
RUN_TEST(upload_compliant_document_passes_clean),
RUN_TEST(upload_gap_document_interrogates_with_rule_message),
RUN_TEST(upload_malformed_extraction_is_rejected_before_persist),
RUN_TEST(upload_unknown_session_is_rejected),
RUN_TEST(user_reply_is_archived_and_acknowledged),
RUN_TEST(request_filing_emits_three_csv_files),
RUN_TEST(filing_after_restart_rehydrates_from_database),
RUN_TEST(request_filing_with_no_invoices_is_an_error_not_a_file),
RUN_TEST(close_session_abandons_unless_filed),
TESTS_END
// ============================================================================
// InvoiceRepository.cpp — SQLite persistence (see header). sqlite3 is the
// amalgamation compiled once into tax_core (backend/include/sqlite3.c).
// ============================================================================

#include "InvoiceRepository.hpp"

#include <stdexcept>
#include <system_error>

#include <filesystem>

#include "sqlite3.h"

namespace tax {

namespace {

// Mirrors database/schema.sql (single canonical source of truth; the column
// `document_json` is documented there). Runs once at first open so the DB is
// self-initializing.
const char* kSchema = R"SQL(
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS tax_sessions (
    session_id          TEXT PRIMARY KEY,
    status              TEXT NOT NULL DEFAULT 'ACTIVE'
        CHECK (status IN ('ACTIVE', 'INTERROGATING', 'FILED', 'ABANDONED')),
    rolling_abstract    TEXT,
    created_at          TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    updated_at          TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);

CREATE TABLE IF NOT EXISTS invoices (
    invoice_id          INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id          TEXT NOT NULL REFERENCES tax_sessions(session_id) ON DELETE CASCADE,
    file_name           TEXT,
    mime_type           TEXT,
    invoice_number      TEXT NOT NULL,
    invoice_date        TEXT NOT NULL,
    seller_gstin        TEXT,
    buyer_gstin         TEXT,
    classification      TEXT NOT NULL DEFAULT 'UNDETERMINED'
        CHECK (classification IN ('B2B', 'B2C', 'UNDETERMINED')),
    supply_type         TEXT NOT NULL
        CHECK (supply_type IN ('INTRA', 'INTER')),
    place_of_supply     TEXT NOT NULL,
    irn_present         INTEGER NOT NULL DEFAULT 0 CHECK (irn_present IN (0, 1)),
    irn                 TEXT,
    hsn_string          TEXT,
    base_value_cents    INTEGER NOT NULL,
    total_tax_cents     INTEGER NOT NULL,
    total_value_cents   INTEGER NOT NULL,
    tax_breakdown_json  TEXT,
    extraction_meta_json TEXT,
    document_json       TEXT,
    extracted_at        TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_invoices_session   ON invoices(session_id);
CREATE INDEX IF NOT EXISTS idx_invoices_number    ON invoices(invoice_number, invoice_date);

CREATE TABLE IF NOT EXISTS line_items (
    line_item_id        INTEGER PRIMARY KEY AUTOINCREMENT,
    invoice_id          INTEGER NOT NULL REFERENCES invoices(invoice_id) ON DELETE CASCADE,
    line_no             INTEGER NOT NULL,
    hsn                 TEXT NOT NULL,
    description         TEXT,
    quantity            REAL,
    unit_price_cents    INTEGER,
    base_value_cents    INTEGER NOT NULL,
    tax_rate_bps        INTEGER NOT NULL,
    note                TEXT
);

CREATE INDEX IF NOT EXISTS idx_line_items_invoice ON line_items(invoice_id);

CREATE TABLE IF NOT EXISTS chat_messages (
    message_id          INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id          TEXT NOT NULL REFERENCES tax_sessions(session_id) ON DELETE CASCADE,
    role                TEXT NOT NULL CHECK (role IN ('assistant', 'user', 'system')),
    content             TEXT NOT NULL,
    action_required     TEXT,
    created_at          TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_chat_session_created ON chat_messages(session_id, created_at);
)SQL";

// RAII prepared statement. Throws with the DB error message on prepare failure.
class Stmt
{
public:
    Stmt(sqlite3* db, const char* sql)
    {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK)
        {
            throw std::runtime_error(std::string("sqlite prepare failed: ") +
                                     sqlite3_errmsg(db) + " [" + sql + "]");
        }
    }
    ~Stmt()
    {
        if (stmt_) sqlite3_finalize(stmt_);
    }
    Stmt(const Stmt&)            = delete;
    Stmt& operator=(const Stmt&) = delete;

    sqlite3_stmt* get() const { return stmt_; }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

std::string err(sqlite3* db) { return sqlite3_errmsg(db) ? sqlite3_errmsg(db) : "unknown error"; }

// ---- binding helpers --------------------------------------------------------

void bindText(sqlite3_stmt* s, int idx, const std::string& v)
{
    sqlite3_bind_text(s, idx, v.c_str(), -1, SQLITE_TRANSIENT);
}
void bindOptionalText(sqlite3_stmt* s, int idx, const std::optional<std::string>& v)
{
    if (v && !v->empty()) bindText(s, idx, *v);
    else                  sqlite3_bind_null(s, idx);
}
void bindBoolInt(sqlite3_stmt* s, int idx, bool v) { sqlite3_bind_int(s, idx, v ? 1 : 0); }
void bindInt64(sqlite3_stmt* s, int idx, std::int64_t v) { sqlite3_bind_int64(s, idx, v); }
void bindDouble(sqlite3_stmt* s, int idx, double v) { sqlite3_bind_double(s, idx, v); }
void bindNull(sqlite3_stmt* s, int idx) { sqlite3_bind_null(s, idx); }

// ---- enum <-> string --------------------------------------------------------

const char* classificationString(Classification c)
{
    switch (c)
    {
        case Classification::B2B:          return "B2B";
        case Classification::B2C:          return "B2C";
        case Classification::UNDETERMINED: return "UNDETERMINED";
    }
    return "UNDETERMINED";
}

const char* supplyTypeString(SupplyType t)
{
    return t == SupplyType::INTRA ? "INTRA" : "INTER";
}

} // anonymous namespace

// ===========================================================================
// construction / destruction
// ===========================================================================

InvoiceRepository::InvoiceRepository(const std::string& db_path)
  : db_(nullptr)
{
    try
    {
        if (!db_path.empty())
        {
            const std::filesystem::path p(db_path);
            if (p.has_parent_path())
            {
                std::error_code ec;
                std::filesystem::create_directories(p.parent_path(), ec);
            }
        }
        if (sqlite3_open(db_path.c_str(), &db_) != SQLITE_OK)
        {
            failClose("cannot open database '" + db_path + "': " + err(db_));
        }
        if (sqlite3_exec(db_, kSchema, nullptr, nullptr, nullptr) != SQLITE_OK)
        {
            failClose("schema initialization failed: " + err(db_));
        }
    }
    catch (...)
    {
        // failClose may throw; make sure the socket/handle is released first.
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        throw;
    }
}

InvoiceRepository::~InvoiceRepository()
{
    if (db_) sqlite3_close(db_);
    db_ = nullptr;
}

void InvoiceRepository::failClose(const std::string& what)
{
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
    throw std::runtime_error(what);
}

// ===========================================================================
// sessions
// ===========================================================================

void InvoiceRepository::createSession(const std::string& session_id)
{
    Stmt s(db_, "INSERT OR IGNORE INTO tax_sessions (session_id) VALUES (?)");
    bindText(s.get(), 1, session_id);
    if (sqlite3_step(s.get()) != SQLITE_DONE) throw std::runtime_error("createSession: " + err(db_));
}

bool InvoiceRepository::sessionExists(const std::string& session_id) const
{
    Stmt s(db_, "SELECT 1 FROM tax_sessions WHERE session_id = ?");
    bindText(s.get(), 1, session_id);
    return sqlite3_step(s.get()) == SQLITE_ROW;
}

void InvoiceRepository::setSessionStatus(const std::string& session_id, const std::string& status)
{
    Stmt s(db_, "UPDATE tax_sessions SET status = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%SZ','now') "
                "WHERE session_id = ?");
    bindText(s.get(), 1, status);
    bindText(s.get(), 2, session_id);
    if (sqlite3_step(s.get()) != SQLITE_DONE) throw std::runtime_error("setSessionStatus: " + err(db_));
}

std::string InvoiceRepository::sessionStatus(const std::string& session_id) const
{
    Stmt s(db_, "SELECT status FROM tax_sessions WHERE session_id = ?");
    bindText(s.get(), 1, session_id);
    if (sqlite3_step(s.get()) == SQLITE_ROW)
    {
        const unsigned char* v = sqlite3_column_text(s.get(), 0);
        return v ? reinterpret_cast<const char*>(v) : "";
    }
    return "";
}

// ===========================================================================
// invoices
// ===========================================================================

std::int64_t InvoiceRepository::saveExtraction(const std::string& session_id,
                                               const std::string& file_name,
                                               const std::string& mime_type,
                                               const StateMatrix& matrix,
                                               const json&        document,
                                               const std::vector<std::string>& extraction_warnings)
{
    // A caller may persist for a session id the pipeline has not finalized yet;
    // an INSERT OR IGNORE guarantees the FK target exists.
    createSession(session_id);

    // extraction metadata: start from the extractor's confidence block, then
    // guarantee a warnings array that also carries any caller-supplied warnings.
    json meta = json::object();
    if (document.contains("extraction_confidence") &&
        document["extraction_confidence"].is_object())
    {
        meta = document["extraction_confidence"];
    }
    if (!meta.contains("warnings") || !meta["warnings"].is_array())
    {
        meta["warnings"] = json::array();
    }
    for (const std::string& w : extraction_warnings)
    {
        meta["warnings"].push_back(w);
    }

    // The verbatim tax breakdown from the extracted document (the RuleEngine
    // audits it against the matrix-derived truth; we preserve the claim as-is).
    std::string breakdown_json = "{}";
    if (document.contains("invoice") && document["invoice"].is_object() &&
        document["invoice"].contains("tax_breakdown") &&
        document["invoice"]["tax_breakdown"].is_object())
    {
        breakdown_json = document["invoice"]["tax_breakdown"].dump();
    }

    Stmt s(db_,
           "INSERT INTO invoices (session_id, file_name, mime_type, invoice_number, "
           "invoice_date, seller_gstin, buyer_gstin, classification, supply_type, "
           "place_of_supply, irn_present, irn, hsn_string, base_value_cents, "
           "total_tax_cents, total_value_cents, tax_breakdown_json, "
           "extraction_meta_json, document_json) "
           "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");

    bindText(s.get(), 1, session_id);
    bindText(s.get(), 2, file_name);
    bindText(s.get(), 3, mime_type);
    bindText(s.get(), 4, matrix.invoiceNumber());
    bindText(s.get(), 5, matrix.invoiceDate());
    bindText(s.get(), 6, matrix.supplierGstin());
    bindOptionalText(s.get(), 7, matrix.buyerGstin());
    bindText(s.get(), 8, classificationString(matrix.classification()));
    bindText(s.get(), 9, supplyTypeString(matrix.supplyType()));
    bindText(s.get(), 10, matrix.placeOfSupply());
    bindBoolInt(s.get(), 11, matrix.irnPresent());
    bindText(s.get(), 12, matrix.irn());
    bindText(s.get(), 13, matrix.hsnString());
    bindInt64(s.get(), 14, static_cast<std::int64_t>(matrix.baseValueCents()));
    bindInt64(s.get(), 15, static_cast<std::int64_t>(matrix.totalTaxCents()));
    bindInt64(s.get(), 16, static_cast<std::int64_t>(matrix.totalValueCents()));
    bindText(s.get(), 17, breakdown_json);
    bindText(s.get(), 18, meta.dump());
    bindText(s.get(), 19, document.dump());   // lossless rehydration source

    if (sqlite3_step(s.get()) != SQLITE_DONE)
    {
        throw std::runtime_error("saveExtraction: " + err(db_));
    }
    const std::int64_t invoice_id = sqlite3_last_insert_rowid(db_);

    // Normalize line items so HSN summaries can aggregate without JSON re-parses.
    int line_no = 0;
    for (const LineItem& li : matrix.lineItems())
    {
        Stmt ls(db_,
                "INSERT INTO line_items (invoice_id, line_no, hsn, description, "
                "quantity, unit_price_cents, base_value_cents, tax_rate_bps) "
                "VALUES (?,?,?,?,?,?,?,?)");
        bindInt64(ls.get(), 1, invoice_id);
        bindInt64(ls.get(), 2, ++line_no);
        bindText(ls.get(), 3, li.hsn);
        bindText(ls.get(), 4, li.description);
        if (li.quantity) bindDouble(ls.get(), 5, *li.quantity);
        else             bindNull(ls.get(), 5);
        if (li.unit_price_cents) bindInt64(ls.get(), 6, static_cast<std::int64_t>(*li.unit_price_cents));
        else                     bindNull(ls.get(), 6);
        bindInt64(ls.get(), 7, static_cast<std::int64_t>(li.base_value_cents));
        bindInt64(ls.get(), 8, li.tax_rate_bps);
        if (sqlite3_step(ls.get()) != SQLITE_DONE)
        {
            throw std::runtime_error("saveExtraction(line): " + err(db_));
        }
    }
    return invoice_id;
}

std::vector<StateMatrix> InvoiceRepository::invoicesForSession(const std::string& session_id) const
{
    Stmt s(db_, "SELECT document_json FROM invoices WHERE session_id = ? "
                "ORDER BY invoice_id ASC");
    bindText(s.get(), 1, session_id);

    std::vector<StateMatrix> out;
    while (sqlite3_step(s.get()) == SQLITE_ROW)
    {
        const unsigned char* text = sqlite3_column_text(s.get(), 0);
        if (!text) continue;
        try
        {
            // Revalidate the stored document through the same fail-fast gate the
            // live pipeline used — a persisted row is only trusted as much as a
            // fresh extraction, and a corrupt row raises instead of silently
            // producing a filing from garbage.
            out.push_back(StateMatrix::validateAndSanitize(
                json::parse(reinterpret_cast<const char*>(text))));
        }
        catch (const std::exception& e)
        {
            throw std::runtime_error("invoicesForSession: stored document failed "
                                     "revalidation: " + std::string(e.what()));
        }
    }
    return out;
}

// ===========================================================================
// chat transcript
// ===========================================================================

void InvoiceRepository::addChatMessage(const std::string& session_id,
                                       const std::string& role,
                                       const std::string& content,
                                       const std::string& action_required)
{
    createSession(session_id);
    Stmt s(db_, "INSERT INTO chat_messages (session_id, role, content, action_required) "
                "VALUES (?,?,?,?)");
    bindText(s.get(), 1, session_id);
    bindText(s.get(), 2, role);
    bindText(s.get(), 3, content);
    bindText(s.get(), 4, action_required);
    if (sqlite3_step(s.get()) != SQLITE_DONE)
    {
        throw std::runtime_error("addChatMessage: " + err(db_));
    }
}

std::vector<ChatMessage> InvoiceRepository::chatHistory(const std::string& session_id) const
{
    Stmt s(db_, "SELECT role, content, action_required, created_at FROM chat_messages "
                "WHERE session_id = ? ORDER BY message_id ASC");
    bindText(s.get(), 1, session_id);

    std::vector<ChatMessage> out;
    while (sqlite3_step(s.get()) == SQLITE_ROW)
    {
        ChatMessage m;
        const unsigned char* role = sqlite3_column_text(s.get(), 0);
        const unsigned char* content = sqlite3_column_text(s.get(), 1);
        const unsigned char* action = sqlite3_column_text(s.get(), 2);
        const unsigned char* created = sqlite3_column_text(s.get(), 3);
        m.role            = role ? reinterpret_cast<const char*>(role) : "";
        m.content         = content ? reinterpret_cast<const char*>(content) : "";
        m.action_required = action ? reinterpret_cast<const char*>(action) : "";
        m.created_at      = created ? reinterpret_cast<const char*>(created) : "";
        out.push_back(std::move(m));
    }
    return out;
}

// ===========================================================================
// diagnostics
// ===========================================================================

std::int64_t InvoiceRepository::sessionCount() const
{
    Stmt s(db_, "SELECT COUNT(*) FROM tax_sessions");
    if (sqlite3_step(s.get()) == SQLITE_ROW) return sqlite3_column_int64(s.get(), 0);
    return 0;
}

} // namespace tax
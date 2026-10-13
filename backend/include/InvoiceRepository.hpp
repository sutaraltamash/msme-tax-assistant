// ============================================================================
// InvoiceRepository — SQLite persistence for sessions, invoices, line items,
// and the chat transcript.
//
// Implements the relational contract in database/schema.sql (the DDL is also
// embedded in InvoiceRepository.cpp so the DB self-initializes on first open).
// Guarantees align with the deterministic design:
//   * MONEY IS INTEGER CENTS — the columns base_value_cents / total_tax_cents /
//     total_value_cents / (line) base_value_cents / unit_price_cents are
//     int64; floats never represent money on disk.
//   * Lossless rehydration — invoices.document_json stores the sanitized
//     extraction document VERBATIM, so invoicesForSession() can rebuild exact
//     StateMatrix values for filing after a process restart (validating the
//     document again, never trusting raw bytes).
//   * Local-only — zero network; the DB lives at DATABASE_PATH on the host.
//
// The repository is NOT thread-safe by itself. The owning Pipeline serializes
// every mutating call behind one mutex; tests use a single thread.
//
// The schema.sql note in the header above the invoices table previewed this
// design; the repository additionally stores extraction metadata and the
// sanitized document per invoice as TEXT columns.
// ============================================================================

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "json.hpp"
#include "StateMatrix.hpp"

struct sqlite3;

namespace tax {

// One archived chat row (transcript continuity / rolling-abstract input).
struct ChatMessage
{
    std::string role;             // assistant | user | system
    std::string content;
    std::string action_required;  // "" when none
    std::string created_at;       // ISO-8601 UTC
};

class InvoiceRepository
{
public:
    // Opens (creating if absent) the SQLite DB at db_path; creates missing
    // parent directories and initializes the schema. Throws std::runtime_error
    // when the database cannot be opened or initialized.
    explicit InvoiceRepository(const std::string& db_path);
    ~InvoiceRepository();

    InvoiceRepository(const InvoiceRepository&)            = delete;
    InvoiceRepository& operator=(const InvoiceRepository&) = delete;

    // ---- sessions ------------------------------------------------------------
    // Inserts a tax_sessions row (status ACTIVE) if not already present.
    void createSession(const std::string& session_id);
    bool sessionExists(const std::string& session_id) const;
    // Idempotent: unknown sessions are skipped (quiet no-op).
    void setSessionStatus(const std::string& session_id, const std::string& status);
    std::string sessionStatus(const std::string& session_id) const;

    // ---- invoices -------------------------------------------------------------
    // Persists one state matrix plus its sanitized source document. Ensures the
    // session row exists (INSERT OR IGNORE) first. Returns the new invoice_id.
    std::int64_t saveExtraction(const std::string& session_id,
                                const std::string& file_name,
                                const std::string& mime_type,
                                const StateMatrix& matrix,
                                const json&        document,
                                const std::vector<std::string>& extraction_warnings);

    // Lossless rehydration of every invoice in the session, in ingestion order.
    std::vector<StateMatrix> invoicesForSession(const std::string& session_id) const;

    // ---- chat transcript ------------------------------------------------------
    void addChatMessage(const std::string& session_id, const std::string& role,
                        const std::string& content,
                        const std::string& action_required = "");
    std::vector<ChatMessage> chatHistory(const std::string& session_id) const;

    // ---- diagnostics ----------------------------------------------------------
    std::int64_t sessionCount() const;

private:
    // Reset the handle (closing any open DB) after a fatal failure.
    void failClose(const std::string& what);

    sqlite3* db_;
};

} // namespace tax
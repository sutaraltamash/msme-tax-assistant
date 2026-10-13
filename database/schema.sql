-- ============================================================================
-- Tax Assistant — SQLite relational schema
-- Initialized at server startup by the persistence layer (Step G).
-- Conventions: TIMESTAMPS stored as ISO-8601 UTC text; MONEY stored as cents
-- (integer, two-decimal scale) so GST splits cannot accumulate float error —
-- part of the deterministic "0% hallucination" guarantee.
-- ============================================================================

PRAGMA foreign_keys = ON;

-- ---------------------------------------------------------------------------
-- A reasoning + filing session. Owns the rolling abstract memory: the C++
-- backend condenses earlier turns into an asynchronous 3-sentence JSON summary
-- stored in rolling_abstract to keep subsequent Gemini prompts compact.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS tax_sessions (
    session_id          TEXT PRIMARY KEY,
    status              TEXT NOT NULL DEFAULT 'ACTIVE'   -- ACTIVE | INTERROGATING | FILED | ABANDONED
        CHECK (status IN ('ACTIVE', 'INTERROGATING', 'FILED', 'ABANDONED')),
    -- Rolling Abstract Memory (architecture.md §1): compacted conversational
    -- history, 3-sentence JSON. NULL until the first compaction.
    rolling_abstract    TEXT,
    created_at          TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now')),
    updated_at          TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);

-- ---------------------------------------------------------------------------
-- One ingested invoice document per session (extracted via Gemini/Mock).
-- Stored fields mirror config/gemini_schema.json so persistence maps 1:1.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS invoices (
    invoice_id          INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id          TEXT NOT NULL REFERENCES tax_sessions(session_id) ON DELETE CASCADE,
    file_name           TEXT,
    mime_type           TEXT,
    invoice_number      TEXT NOT NULL,
    invoice_date        TEXT NOT NULL,                  -- YYYY-MM-DD
    seller_gstin        TEXT,
    buyer_gstin         TEXT,                            -- NULL => B2C / undetermined
    classification      TEXT NOT NULL DEFAULT 'UNDETERMINED'
        CHECK (classification IN ('B2B', 'B2C', 'UNDETERMINED')),
    supply_type         TEXT NOT NULL                    -- INTRA | INTER
        CHECK (supply_type IN ('INTRA', 'INTER')),
    place_of_supply     TEXT NOT NULL,
    irn_present         INTEGER NOT NULL DEFAULT 0 CHECK (irn_present IN (0, 1)),
    irn                 TEXT,
    hsn_string          TEXT,
    base_value_cents    INTEGER NOT NULL,                -- total taxable base
    total_tax_cents     INTEGER NOT NULL,
    total_value_cents   INTEGER NOT NULL,
    tax_breakdown_json  TEXT,                            -- verbatim extracted breakdown
    extraction_meta_json TEXT,                           -- confidence/warnings from extractor
    document_json       TEXT,                            -- sanitized extraction doc, verbatim: lossless StateMatrix rehydration for filing after restart
    extracted_at        TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_invoices_session   ON invoices(session_id);
CREATE INDEX IF NOT EXISTS idx_invoices_number    ON invoices(invoice_number, invoice_date);

-- ---------------------------------------------------------------------------
-- Line items, normalized out of the invoice JSON. Kept so GSTR-1 HSN summary
-- (Phase 2 L step) can aggregate by HSN without re-parsing JSON.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS line_items (
    line_item_id        INTEGER PRIMARY KEY AUTOINCREMENT,
    invoice_id          INTEGER NOT NULL REFERENCES invoices(invoice_id) ON DELETE CASCADE,
    line_no             INTEGER NOT NULL,
    hsn                 TEXT NOT NULL,
    description         TEXT,
    quantity            REAL,
    unit_price_cents    INTEGER,
    base_value_cents    INTEGER NOT NULL,
    tax_rate_bps        INTEGER NOT NULL,                -- basis points, e.g. 1800 = 18%
    note                TEXT
);

CREATE INDEX IF NOT EXISTS idx_line_items_invoice ON line_items(invoice_id);

-- ---------------------------------------------------------------------------
-- Streaming chat transcript: assistant interrogation prompts and user replies.
-- ore of the Rolling Abstract Memory compaction.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS chat_messages (
    message_id          INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id          TEXT NOT NULL REFERENCES tax_sessions(session_id) ON DELETE CASCADE,
    role                TEXT NOT NULL CHECK (role IN ('assistant', 'user', 'system')),
    content             TEXT NOT NULL,
    action_required     TEXT,                            -- e.g. INPUT_TEXT when awaiting user resolution
    created_at          TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_chat_session_created ON chat_messages(session_id, created_at);
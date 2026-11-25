# Final Documentation Sheet — AI-Powered Smart Tax Compliance Assistant

> Generated from source code only. No git commands, no builds, no external docs.

---

## 1. Executive Ground Truth & Current SDLC Status

| Aspect | Reality (from code) |
|--------|---------------------|
| **Language / Standard** | C++17 (MSVC on Windows, GCC/Clang elsewhere) |
| **Build System** | CMake 3.15+ → `tax_core` static lib + `tax_assistant_server` exe |
| **Web Framework** | Crow (header-only, bundled under `backend/include/crow/`) + vendored standalone Asio |
| **HTTP Transport** | **Windows:** WinHTTP (Schannel TLS) via `WinHttpClient.cpp`; **Linux/macOS:** libcurl via `CurlHttpClient.cpp` |
| **Database** | SQLite3 (amalgamation compiled into `tax_core`) |
| **JSON Library** | nlohmann/json (single header under `backend/include/json.hpp`) |
| **Default Groq Model** | `qwen/qwen3.8-27b` (from `.env.example`, **not** `llama-3.2-11b-vision-preview` as claimed in CLAUDE.md) |
| **Default Port** | **18080** (`.env.example` + `main.cpp`), not 8080 |
| **AI_MODE** | `live` (uses Groq) or anything else → `MockExtractor` (offline) |
| **Frontend** | Single-page vanilla JS + HTML5/CSS3 served statically by Crow; copied to build dir at `POST_BUILD` |
| **Rule Engine** | Configuration-driven from `config/rules_2026.json` (no recompile needed) |
| **Deterministic Math** | Integer-cent (paisa) with basis-point rates; **0% floating-point** in tax splits |

### Complete vs. Stubbed / Planned

| Feature | Status | Evidence |
|---------|--------|----------|
| **Multi-turn Rolling Abstract Memory** | **STUBBED** — `tax_sessions.rolling_abstract` column exists (schema.sql:22) but **no compaction code** in `Pipeline.cpp` or `InvoiceRepository.cpp` |
| **USER_REPLY re-evaluates rules** | **NOT IMPLEMENTED** — `processUserReply()` only archives the reply (`Pipeline.cpp:534-565`), **does not** re-run `RuleEngine::evaluate()` or persist corrections back to the invoice |
| **Groq PDF support** | **NOT SUPPORTED** — `GroqClient.cpp:128-133` throws for `application/pdf`; frontend must pre-render (PDF.js) |
| **Docker / Nginx (Step 5)** | **PAUSED / NOT STARTED** — no `Dockerfile`, no `nginx.conf`, no compose |
| **Admin UI** | **NOT PRESENT** — `role` column exists (`users.role`), seed logic in `InvoiceRepository.cpp:385-413`, but **no admin-only routes or pages** |
| **Cold-start rehydration** | **WORKING** — `requestFiling()` loads `document_json` → `StateMatrix::validateAndSanitize()` on restart (`Pipeline.cpp:590-593`) |
| **Multi-slab per invoice** | **WORKING** — `GSTRGenerator::recomputeSplit()` computes CGST/SGST/IGST per line item by `tax_rate_bps` |
| **GSTR-3B aggregation** | **WORKING** — buckets by INTRA/INTER (`GSTRGenerator.cpp:344-358`) |

### Key Outdated Claims in Old Docs

| Claim (CLAUDE.md / README) | Actual Code |
|----------------------------|-------------|
| Port **8080** | **18080** |
| Groq model **llama-3.2-11b-vision-preview** | **qwen/qwen3.8-27b** |
| `Rolling Abstract Memory` "compresses conversation history" | Column exists; **no compaction logic** |
| `USER_REPLY` triggers rule re-evaluation | **Only archives**; no re-eval |
| `E-invoicing mandatory for AATO > ₹5 Crore` | Rule **R-EINV-001** checks `supplier.aato_crore` but extractor rarely provides it |
| `Secure` cookie flag | **Commented out** (`main.cpp:288`) — only `HttpOnly; SameSite=Lax` |

---

## 2. System Architecture & End-to-End Flows

### 2.1 Auth Lifecycle

```
REGISTER
  POST /api/auth/register  {username, email, password}
  → rate limit (10/min/IP)
  → validate email format, password ≥8 chars
  → reject if email == ADMIN_EMAIL
  → PBKDF2-HMAC-SHA256 (100k iters, 32B salt) → password_hash
  → INSERT users (role='user')
  → generate 32B session token → SHA-256 hash → store in sessions
  → Set-Cookie: gstnode_session=<hex>; HttpOnly; SameSite=Lax; Max-Age=86400
  → 200 {username, email, role:"user"}

LOGIN
  POST /api/auth/login  {email, password, remember?}
  → rate limit (10/min/IP)
  → lookup user by normalized email
  → constant-time verifyPassword()
  → create session (24h or 30d if remember)
  → Set-Cookie (same flags)
  → 200 {username, email, role}

LOGOUT
  POST /api/auth/logout
  → delete session by cookie hash
  → Set-Cookie: deleted; Max-Age=0

ME
  GET /api/auth/me
  → validate cookie → session → user
  → 200 {username, email, role}

ADMIN SEED (startup)
  main.cpp:327-337 → repo.seedAdminIfNeeded(ADMIN_EMAIL, ADMIN_PASSWORD)
  → creates/updates user with role='admin', password from env
```

### 2.2 Invoice Upload → Extraction → Validation → Persistence

```
Client (WS)                          Server
──────────────────────────────────────────────────────────────
1. UPLOAD_INVOICE {payload: base64, mime_type}
   │
   ├── [WS onaccept] validates gstnode_session cookie → rejects if invalid/expired
   │
   ├── Rate limit: 5 uploads/min/IP (uploadLimiter)
   │
   ├── base64Decode → raw bytes
   │
   ├── Size check: ≤ 5 MB raw
   │
   ├── Magic bytes verify:
   │     %PDF → application/pdf
   │     89 50 4E 47... → image/png
   │     FF D8 FF → image/jpeg
   │     else → reject
   │
   ├── SecureTempFile (RAII, random name under %TEMP%/gstnode_uploads/)
   │
   ├── Extractor (GroqClient or MockExtractor)
   │     Groq: POST https://api.groq.com/openai/v1/chat/completions
   │           - model from GROQ_MODEL
   │           - temperature=0
   │           - image as data: URL
   │           - system prompt = compacted groq_schema.json + security delimiters
   │           - retries 3× on 429/503 with backoff
   │     Mock: deterministic fixture matching schema
   │
   ├── StateMatrix::validateAndSanitize(doc)  // throws on schema violation
   │     - GSTIN normalize (uppercase, trim)
   │     - State code normalize (2-char)
   │     - HSN normalize (digits only, first valid from CSV)
   │     - Money → Cents (round-half-up)
   │     - Rate % → basis points (×100)
   │     - Validate GST slabs: {0, 25, 300, 500, 1200, 1800, 2800} bps
   │     - Supply type: INTRA/INTER
   │     - Classification: B2B/B2C/UNDETERMINED
   │     - Tax breakdown: derive has_cgst_sgst/has_igst from AMOUNTS (not booleans)
   │
   ├── RuleEngine::evaluate(matrix, raw_doc)
   │     - Loads rules from config/rules_2026.json
   │     - Resolves paths against raw_doc + matrix-derived truth
   │     - Checks: present/absent/eq/neq/gt/gte/lt/lte/eq_true/eq_false/token_length_out/ne_cross
   │     - "when" precondition skips entire rule
   │     - Composite checks with AND/OR nesting
   │     - Template substitution for messages ({hsn}, {line_index}, {aato_crore}...)
   │
   ├── For each violated rule → CHAT_MESSAGE {role:"assistant", content:template, action_required:"INPUT_TEXT"}
   │
   ├── Persist:
   │     tax_sessions (status=INTERROGATING or ACTIVE)
   │     invoices (all fields + document_json = lossless raw_doc)
   │     line_items (normalized per line)
   │     chat_messages (assistant gaps + user replies later)
   │
   └── LEDGER_UPDATE frame → frontend renders ledger immediately
2. USER_REPLY {text}
   → rate limit: 20/min/IP (chatLimiter)
   → sanitizeUserMessage() (strip controls)
   → archive to chat_messages (role=user)
   → ack only ("Your note was recorded... live Groq chat loop (Step G) will apply...")
   → **NO re-evaluation of rules** (stub)
3. REQUEST_FILING
   → rate limit: 20/min/IP (chatLimiter)
   → load invoices from memory or repo (cold start: document_json → validateAndSanitize)
   → GSTRGenerator::gstr1B2B, gstr1HsnSummary, gstr3B
   → FILING_READY {files: {gstr1_b2b.csv, gstr1_hsn_summary.csv, gstr3b_summary.csv}}
   → session status = FILED
```

### 2.3 Multi-Bill CSV Generation

| CSV | Rows | Key Columns | Notes |
|-----|------|-------------|-------|
| `gstr1_b2b.csv` | 1 per line item (B2B only) | gstin_recipient, invoice_number, invoice_date (DD-MMM-YYYY), invoice_value, place_of_supply (Code-StateName), supply_type (Intra/Inter), gst_rate, taxable_value, igst_amt, cgst_amt, sgst_amt, cess_amt | Recomputes statutory split per line; invoice_value = sum(line taxable+tax) |
| `gstr1_hsn_summary.csv` | 1 per (hsn, gst_rate) aggregate | hsn_code, gst_rate, taxable_value, igst_amt, cgst_amt, sgst_amt, cess_amt, quantity | Aggregates across all invoices |
| `gstr3b_summary.csv` | 2 rows max (4A-Intra, 4A-Inter) | section, description, taxable_value, igst_amt, cgst_amt, sgst_amt, cess_amt, total_tax_amt | Buckets by SupplyType |

### 2.4 HTTP Route Table

| Method | Path | Auth? | Rate-Limited? | Purpose |
|--------|------|-------|---------------|---------|
| GET | `/` | No | No | Serves `login.html` |
| GET | `/login.html` | No | No | Login page |
| GET | `/dashboard.html` | **Yes** (cookie) | No | Serves `index.html` (SPA) |
| GET | `/index.html` | **Yes** | No | Alias for dashboard |
| GET | `/styles.css` | No | No | Stylesheet |
| GET | `/favicon.ico` | No | No | Favicon |
| GET | `/assets/<path>` | No | No | Static assets (path traversal blocked) |
| POST | `/api/auth/register` | No | **Yes** (10/min/IP) | Create user + session |
| POST | `/api/auth/login` | No | **Yes** (10/min/IP) | Verify password + session |
| POST | `/api/auth/logout` | No (cookie) | No | Delete session |
| GET | `/api/auth/me` | **Yes** (cookie) | No | Current user info |
| WS | `/ws` | **Yes** (cookie in handshake) | Per-message (see below) | Full-duplex pipeline |

### 2.5 WebSocket Contract

#### Client → Server

| `type` | Fields | Rate Limit |
|--------|--------|------------|
| `UPLOAD_INVOICE` | `payload` (base64), `mime_type` (optional) | 5/min/IP |
| `USER_REPLY` | `text` (≤ 2000 chars) | 20/min/IP |
| `REQUEST_FILING` | (none) | 20/min/IP |

#### Server → Client

| `type` | Fields |
|--------|--------|
| `SESSION_INIT` | `session_id` |
| `CHAT_MESSAGE` | `role` ("assistant"\|"user"\|"system"), `content`, `action_required` ("INPUT_TEXT" or absent) |
| `LEDGER_UPDATE` | `supplier{legal_name,gstin,state_code}`, `buyer{legal_name?,gstin?,state_code?}`, `invoice{invoice_number,invoice_date,place_of_supply}`, `irp{irn?}`, `line_items[{hsn_code,description,quantity,unit_price_cents,taxable_amount_cents,cgst_rate_bps,sgst_rate_bps,igst_rate_bps,total_cents}]`, `totals{base_value_cents,igst_cents,cgst_cents,sgst_cents,total_tax_cents,total_value_cents}` |
| `FILING_READY` | `files{gstr1_b2b.csv, gstr1_hsn_summary.csv, gstr3b_summary.csv}` (CSV text) |

---

## 3. Reconciled Database Architecture

> Tables created by **both** `database/schema.sql` (canonical) **and** `InvoiceRepository.cpp:kSchema` (identical at runtime). All money in **cents (int64)**. Rates in **basis points (int)**. Timestamps = ISO-8601 UTC text.

| Table | Columns | PK / FK / Constraints |
|-------|---------|----------------------|
| `tax_sessions` | `session_id` TEXT, `status` TEXT CHECK(ACTIVE\|INTERROGATING\|FILED\|ABANDONED), `rolling_abstract` TEXT, `created_at` TEXT, `updated_at` TEXT | PK=`session_id` |
| `users` | `id` INTEGER AUTOINC, `username` TEXT, `email` TEXT UNIQUE, `password_hash` TEXT, `role` TEXT DEFAULT 'user', `message_count` INTEGER DEFAULT 0, `created_at` TEXT | PK=`id`, UNIQUE=`email` |
| `sessions` (auth) | `token_hash` TEXT, `user_id` INTEGER, `expires_at` INTEGER (epoch), `created_at` INTEGER DEFAULT (strftime('%s','now')) | PK=`token_hash`, FK=`user_id→users.id` ON DELETE CASCADE, INDEX(`user_id`), INDEX(`expires_at`) |
| `invoices` | `invoice_id` INTEGER AUTOINC, `session_id` TEXT, `file_name` TEXT, `mime_type` TEXT, `invoice_number` TEXT, `invoice_date` TEXT (YYYY-MM-DD), `seller_gstin` TEXT, `buyer_gstin` TEXT, `classification` TEXT CHECK(B2B\|B2C\|UNDETERMINED), `supply_type` TEXT CHECK(INTRA\|INTER), `place_of_supply` TEXT, `irn_present` INTEGER(0\|1), `irn` TEXT, `hsn_string` TEXT, `base_value_cents` INTEGER, `total_tax_cents` INTEGER, `total_value_cents` INTEGER, `tax_breakdown_json` TEXT, `extraction_meta_json` TEXT, `document_json` TEXT, `extracted_at` TEXT | PK=`invoice_id`, FK=`session_id→tax_sessions.session_id` ON DELETE CASCADE, INDEX(`session_id`), INDEX(`invoice_number, invoice_date`) |
| `line_items` | `line_item_id` INTEGER AUTOINC, `invoice_id` INTEGER, `line_no` INTEGER, `hsn` TEXT, `description` TEXT, `quantity` REAL, `unit_price_cents` INTEGER, `base_value_cents` INTEGER, `tax_rate_bps` INTEGER, `note` TEXT | PK=`line_item_id`, FK=`invoice_id→invoices.invoice_id` ON DELETE CASCADE, INDEX(`invoice_id`) |
| `chat_messages` | `message_id` INTEGER AUTOINC, `session_id` TEXT, `role` TEXT CHECK(assistant\|user\|system), `content` TEXT, `action_required` TEXT, `created_at` TEXT | PK=`message_id`, FK=`session_id→tax_sessions.session_id` ON DELETE CASCADE, INDEX(`session_id, created_at`) |

### Cold-Start Rehydration (Pipeline.cpp:590-593)
```
requestFiling(session_id)
  → if session not in memory:
      repo.sessionExists(id) ? adopt : reject
      invoices = repo.invoicesForSession(id)
        → SELECT document_json FROM invoices WHERE session_id=?
        → json::parse → StateMatrix::validateAndSanitize() (fail-fast)
  → generate CSVs from rehydrated StateMatrix vector
```

---

## 4. Deterministic GST Engine & GSTR CSV Spec

### 4.1 StateMatrix OCR Normalization & Integer-Cent Math

| Operation | Code Location | Detail |
|-----------|---------------|--------|
| Money → Cents | `StateMatrix.cpp:172-189` | `round(d * 100.0)` with `llround`, range check ≤ 1e15 |
| Rate % → bps | `StateMatrix.cpp:34-51` | `llround(pct * 100.0)` → validates against `{0,25,300,500,1200,1800,2800}` |
| Tax on base | `StateMatrix.cpp:191-211` | `(base_cents * rate_bps + 5000) / 10000` — **round-half-up** to nearest paisa |
| INTRA split | `StateMatrix.cpp:217-237` | `cgst = taxOn(base, rate/2)`, `sgst = taxOn(base, rate/2)` — **independent per-head rounding** |
| INTER split | same | `igst = taxOn(base, rate)` |
| GSTIN normalize | `StateMatrix.cpp:72-79` | trim → uppercase |
| State code normalize | `StateMatrix.cpp:91-100` | extract leading 2 digits if numeric prefix |
| HSN normalize | `StateMatrix.cpp:102-132` | CSV → first valid digits-only token (≤8 digits); else compact digits |
| Placeholder detect | `StateMatrix.cpp:81-89` | empty, n/a, na, none, null, nil, -, unregistered* → treated as absent |

### 4.2 RuleEngine — All 7 Rules from `config/rules_2026.json`

| Rule ID | Category | Severity | When Precondition | Violation Logic (Predicate = TRUE) | Gap Resolution | Action |
|---------|----------|----------|-------------------|------------------------------------|----------------|--------|
| **R-EINV-001** | e_invoice | error | `supplier.aato_crore` present | `aato_crore > 5` **AND** `invoice.irn_present == false` | `irn_or_exemption` | INPUT_TEXT |
| **R-IRP-002** | irp | error | `invoice.irn_present == true` | `irp.reported_on` **absent** | `irp_reported_on` | INPUT_TEXT |
| **R-SPLIT-003** | tax_split | error | (none) | `place_of_supply == "same_as_supplier_state"` **AND** `tax_breakdown.has_igst == true` | `tax_allocation` | INPUT_TEXT |
| **R-SPLIT-004** | tax_split | error | (none) | `place_of_supply != "same_as_supplier_state"` **AND** `tax_breakdown.has_cgst_sgst == true` | `tax_allocation` | INPUT_TEXT |
| **R-HSN-005** | hsn | warning | (none) | For **each line item**: `hsn.length ∉ {4,6,8}` | `correct_hsn` | INPUT_TEXT |
| **R-CHRG-006** | tax_split | warning | `tax_breakdown.has_igst == false` | `cgst absent` **OR** `sgst absent` **OR** `sgst != cgst` (cross-field) | `tax_allocation` | INPUT_TEXT |
| **R-CLASS-007** | classification | info | (none) | `classification == "UNDETERMINED"` **OR** (`classification != "B2C"` **AND** `buyer.gstin absent`) | `buyer_registration` | INPUT_TEXT |

**Evaluation Order** (RuleEngine.cpp:156-160): sequential as in JSON array.  
**Composite Checks**: nested `{operator:"AND"|"OR", checks:[...]}` supported (R-CLASS-007 uses this).  
**Threshold Single-Sourcing**: `source: "thresholds.e_invoicing_aato_crore"` overrides literal `value` (RuleEngine.cpp:287-297).  
**Matrix-Derived Truth Wins**: `invoice.tax_breakdown.has_igst` and `has_cgst_sgst` resolved from **computed amounts**, not payload booleans (RuleEngine.cpp:218-226).

### 4.3 GSTRGenerator — Exact CSV Spec

| Aspect | Implementation |
|--------|----------------|
| **Date format** | `DD-MMM-YYYY` via `formatInvoiceDate()` (GSTRGenerator.cpp:81-111) — `2026-11-12 → 12-Nov-2026` |
| **Place of Supply** | `Code-StateName` via `formatPlaceOfSupply()` (GSTRGenerator.cpp:116-203) — `27 → 27-Maharashtra`, `MH → 27-Maharashtra` |
| **Rate display** | Whole percent if `rate_bps % 100 == 0` else 2-decimal (`1800→"18"`, `25→"0.25"`) |
| **Money display** | `centsToRupees()` → `"1000.00"` (exactly 2 decimals) |
| **CSV escaping** | RFC-4180: quote if contains `, " \n \r`; double internal quotes |
| **Formula injection neutralize** | `Sanitizer::neutralizeCsvFormula()` — prepend `'` if starts with `= + - @ \t \r` |
| **Multi-slab splitting** | `recomputeSplit(matrix, line_item)` → `StateMatrix::computeTaxSplit()` per line |
| **GSTR-3B aggregation** | Two buckets: `INTRA` → section `4A-Intra`, `INTER` → `4A-Inter`; sums taxable, IGST, CGST, SGST, cess separately |

---

## 5. Core Backend Module Inventory

| File | Role | Key Functions / Types | Dependencies |
|------|------|----------------------|--------------|
| `main.cpp` | Entrypoint, HTTP/Ws server, auth routes, rate limiters, static file serving | `makeExtractor()`, `Pipeline` wiring, `RateLimiter` (sliding window), auth endpoints, WS `.onaccept()/.onopen()/.onmessage()/.onclose()` | Crow, all backend headers, `crypto`, `Pipeline`, `InvoiceRepository` |
| `Pipeline.cpp` | End-to-end orchestration: upload → extract → validate → evaluate → persist → filing | `processUpload()`, `processUserReply()`, `requestFiling()`, `ledgerUpdateFrame()`, `openSession()/closeSession()`, `loadRulesConfig()`, `uuid4()`, `verifyMagicBytes()`, `SecureTempFile`, `base64Decode()` | `StateMatrix`, `RuleEngine`, `GSTRGenerator`, `InvoiceRepository`, `IExtractor`, `Sanitizer`, `Crypto` |
| `InvoiceRepository.cpp` | SQLite persistence (schema, sessions, users, invoices, lines, chat) | `createUser()`, `verifyUser()`, `seedAdminIfNeeded()`, `createAuthSession()/validateAuthSession()`, `saveExtraction()`, `invoicesForSession()` (rehydration), `addChatMessage()`, `chatHistory()` | SQLite3 (amalgamation), `Crypto`, `StateMatrix` (for revalidation) |
| `StateMatrix.cpp` | Immutable tax ledger + deterministic math + schema validation | `validateAndSanitize()`, `validateAgainstSchema()`, `computeTaxSplit()`, `money::toCents()`, `money::taxOn()`, normalization helpers (GSTIN, state, HSN), rate validation | nlohmann/json, `Sanitizer` |
| `RuleEngine.cpp` | Config-driven rule evaluation (DSL from rules_2026.json) | `evaluate()`, `evalRule()`, `evalCheck()`, `resolvePath()`, `resolveFact()`, `substitute()` (template), `collectFacts()`, `resolveThreshold()` | `StateMatrix`, nlohmann/json |
| `GSTRGenerator.cpp` | Deterministic CSV serialization for GSTR-1 B2B, HSN Summary, GSTR-3B | `gstr1B2B()`, `gstr1HsnSummary()`, `gstr3B()`, `placeOfSupplyCode()`, `recomputeSplit()`, `centsToRupees()`, `formatInvoiceDate()`, `formatPlaceOfSupply()`, `csvEscaped()` | `StateMatrix`, `Sanitizer` |
| `GroqClient.cpp` | HTTPS client for Groq Cloud API (multimodal vision) | `extractFromBuffer()`, `buildRequest()`, `parseSuccessResponse()`, `stripCodeFence()`, retry logic (429/503), schema validation post-extraction | `IHttpClient` (WinHTTP/curl), nlohmann/json, `StateMatrix` (for validation), `Sanitizer` |
| `MockExtractor.cpp` | Offline deterministic extractor for Phase 1 / testing | `extractFromBuffer()` → returns fixture matching `groq_schema.json` | nlohmann/json |
| `Crypto.cpp` | PBKDF2-HMAC-SHA256, session tokens, constant-time compare | `pbkdf2Hash()`, `verifyPassword()`, `generateSalt()`, `generateSessionToken()`, `hashSessionToken()`, `bytesToHex()`, `hexToBytes()`, `constantTimeEqual()` | Windows: BCrypt; else: OpenSSL (not vendored — assumes system) |
| `Sanitizer.cpp` | Input sanitization: invisible chars, CSV formula neutralization, prompt injection delimiters | `stripInvisibleAndControls()`, `sanitizeUserMessage()`, `neutralizeCsvFormula()`, `wrapUntrusted()` | `<cctype>`, `<string>` |
| `WinHttpClient.cpp` / `CurlHttpClient.cpp` | `IHttpClient` implementations for HTTPS POST | `postJson(url, api_key, payload, timeout)` → `HttpResponse{status, body, error}` | WinHTTP (Windows) / libcurl (else) |

---

---

## 6. Security & Hardening Audit

### 6.1 Cryptography (Crypto.cpp)

| Mechanism | Implementation | Details |
|-----------|----------------|---------|
| **Password Hashing** | PBKDF2-HMAC-SHA256 | 100,000 iterations, 16-byte salt, 32-byte derived key. Format: `pbkdf2_sha256$<iters>$<hex_salt>$<hex_hash>` |
| **Password Verification** | Constant-time compare | `constantTimeEqual()` on raw hex strings (same length check + bitwise XOR) |
| **Session Tokens** | 32-byte cryptographically random | `generateSessionToken()` → `randomBytes()` (BCryptGenRandom on Windows, `/dev/urandom` on POSIX) |
| **Token Storage** | SHA-256 hash only | `hashSessionToken()` stores `sha256(token)` in `sessions.token_hash`; raw token never persisted |
| **SHA-256** | Self-contained, no OpenSSL | Full FIPS-180-4 implementation in `sha256_raw()` |
| **HMAC-SHA256** | RFC 2104 | Used by PBKDF2 internally |

### 6.2 Session & Cookie Security (main.cpp)

| Aspect | Status | Code Location |
|--------|--------|---------------|
| **HttpOnly** | ✅ Set | `makeSessionCookie()` line 286 |
| **SameSite** | ✅ `Lax` | `makeSessionCookie()` line 286 |
| **Secure** | ❌ **Commented out** | `main.cpp:288` — `// if (is_production) oss << "; Secure";` |
| **Path** | ✅ `/` | `makeSessionCookie()` line 286 |
| **Max-Age** | ✅ 24h / 30d | Configurable via `remember` flag |
| **Cookie Name** | `gstnode_session` | Hex-encoded 32-byte token |

### 6.3 WebSocket Authentication (main.cpp:679-708)

```cpp
.onaccept([&repo](const crow::request& req, void**) -> bool {
    std::string tokenHex = getCookie(req, "gstnode_session");
    if (tokenHex.empty()) return false;
    auto tokenBytes = crypto::hexToBytes(tokenHex);
    std::string tokenHash = crypto::hashSessionToken(tokenBytes);
    std::int64_t user_id, expires_at;
    if (!repo.validateAuthSession(tokenHash, user_id, expires_at)) return false;
    if (expires_at < nowEpoch()) { repo.deleteAuthSession(tokenHash); return false; }
    return true;
})
```
- **Validates on handshake** — rejects before `.onopen()`
- **Checks expiry** — deletes expired sessions proactively
- **No user_id binding to WS session** — `Pipeline` tracks by `session_id` only; **no cross-reference** to auth `user_id`

### 6.4 Rate Limiting (main.cpp:139-232)

| Limiter | Window | Max Requests | Applied To |
|---------|--------|--------------|------------|
| `chatLimiter` | 60 sec | 20 | `USER_REPLY`, `REQUEST_FILING` |
| `uploadLimiter` | 60 sec | 5 | `UPLOAD_INVOICE` |
| `authLimiter` | 60 sec | 10 | `/api/auth/register`, `/api/auth/login` |

- **In-memory sliding window** per IP (`std::unordered_map<string, deque<time_point>>`)
- **No persistence** — resets on server restart
- **IP extraction** via `X-Forwarded-For` header (falls back to `127.0.0.1`)

### 6.5 File Upload Hardening (Pipeline.cpp:395-430, file_handler.js:124-130)

| Check | Limit | Enforcement Point |
|-------|-------|-------------------|
| **Magic bytes** | PDF: `%PDF`, PNG: `89 50 4E 47...`, JPEG: `FF D8 FF` | Server (`verifyMagicBytes`) + Client (`verifyMagicBytes`) |
| **Max file size** | 5 MB raw (≈6.7 MB base64) | Server (decoded bytes) + Client (File.size) |
| **Max prompt length** | 2000 chars | Server (`USER_REPLY`) |
| **Max files/batch** | 5 | Client (`file_handler.js:13`) |
| **Temp file** | RAII `SecureTempFile` in `%TEMP%/gstnode_uploads/` | Server (`Pipeline.cpp:126-164`) — auto-cleanup on scope exit |

### 6.6 Static Asset Path Traversal (main.cpp:359-373)

```cpp
CROW_ROUTE(app, "/assets/<path>")([staticDir](const std::string& path) {
    if (path.find("..") != string::npos || path.find('\\') != string::npos || 
        (path.size() > 0 && path[0] == '/')) {
        return 400 { "error": "Invalid asset path" };
    }
    res.set_static_file_info(staticDir + "/" + path);
    return res;
});
```
- Blocks `..`, backslash, leading slash
- No canonicalization — **relative paths outside `staticDir` still possible if symlinks exist**

### 6.7 Prompt Injection Defenses (Sanitizer.cpp, GroqClient.cpp)

| Defense | Implementation |
|---------|----------------|
| **Delimiter wrapping** | `<untrusted_invoice_data>...</untrusted_invoice_data>` around image payload (GroqClient.cpp:226-229) |
| **Closing delimiter escape** | `</untrusted_...>` → `<\/untrusted_...>` in user/invoice text (Sanitizer.cpp:136-157) |
| **Invisible char stripping** | Zero-width (U+200B-U+200F), bidi overrides (U+202A-U+202E), BOM (U+FEFF) |
| **Control char stripping** | C0 (0x00-0x1F) except `\n` `\r` `\t`, plus DEL (0x7F) |
| **Byte cap with truncation notice** | `enforceByteCap()` appends ` …[TRUNCATED]` |

### 6.8 CSV Formula Injection Neutralization (Sanitizer.cpp:169-182, GSTRGenerator.cpp:47-63)

```cpp
// Sanitizer.cpp
std::string neutralizeCsvFormula(std::string_view field) {
    char first = field[0];
    if (first == '=' || first == '+' || first == '-' || first == '@' ||
        first == '\t' || first == '\r') {
        return "'" + string(field);  // Prepend single quote
    }
    return string(field);
}
// GSTRGenerator.cpp::csvEscaped() calls neutralizeCsvFormula() before RFC-4180 quoting
```

### 6.9 Admin vs. User Role Enforcement

| Check | Status | Evidence |
|-------|--------|----------|
| `role` column in `users` | ✅ | `InvoiceRepository.cpp:40` |
| Admin seed on startup | ✅ | `InvoiceRepository.cpp:385-413` |
| Admin-only routes | ❌ **NONE** | No route checks `role == "admin"` |
| Admin UI / pages | ❌ **NONE** | Frontend has no admin panel |
| Role returned in `/api/auth/me` | ✅ | `main.cpp:613-617` |
| Role used for authorization | ❌ | Only used for display |

---

## 7. Frontend & UI Breakdown

### 7.1 File Roles

| File | Purpose |
|------|---------|
| `login.html` | Auth page: login/register forms, POSTs to `/api/auth/login|register`, redirects to `dashboard.html` on success |
| `index.html` | SPA shell (served at `/dashboard.html` & `/index.html`): header, ledger panel, chat panel, filing section, modals |
| `app.js` | Main controller: WS client init, ledger rendering, chat handling, filing trigger, CSV preview/download modals, auth check on load |
| `file_handler.js` | Drag-drop/file-input → Base64 encoder + client-side magic byte verify + **PDF.js auto-crop JPEG pipeline** |
| `websocket_client.js` | `TaxWebSocketClient` class: reconnect logic, frame dispatch (`onSessionInit`, `onChatMessage`, `onLedgerUpdate`, `onFilingReady`), send helpers |

### 7.2 Key UI Flows

#### Invoice Upload (file_handler.js → app.js)
```
1. User drops/selects file(s) (max 5, 5 MB each)
2. file_handler.processFile():
   - size check
   - mime type from extension (fallback)
   - arrayBuffer → Uint8Array header → verifyMagicBytes()
   - If PDF:
       * Load pdf.min.js / pdf.worker.min.js
       * Render up to 3 pages at scale ≤1.5, max 900px
       * AUTO-CROP: scan rows for non-white pixels (RGB < 245), crop top/bottom with 16px padding
       * Stack pages vertically on final canvas
       * toDataURL('image/jpeg', 0.82) → base64
   - If PNG/JPEG: direct base64
3. onFileSelected callback → app.js uploadModal hidden, status text, chat message appended
4. wsClient.uploadInvoice(base64, mimeType) → WS UPLOAD_INVOICE frame
```

#### Ledger Rendering (app.js:143-218)
- `onLedgerUpdate(data)` → `renderLedger(matrixData)`
- Populates: supplier/buyer boxes, invoice meta, IRN status, line items table (HSN, desc, qty, unit price, taxable, CGST/SGST/IGST rates, total), totals grid
- Supply type badge: `Inter-State (IGST)` vs `Intra-State (CGST + SGST)` derived from supplier state vs place_of_supply

#### Chat Interrogation (app.js:99-120, 226-240)
- `appendChatMessage(role, content, actionRequired)` → bubbles with role label (🤖/👤)
- `actionRequired` focuses chat input (no visual badge rendered)
- Error detection re-enables filing button if it was disabled mid-generation

#### Filing Generation & CSV Preview/Download (app.js:406-474)
```
btnRequestFiling.click()
  → disable button, "Generating GSTR CSVs..."
  → wsClient.requestFiling() → WS REQUEST_FILING
  → onFilingReady(files):
       generatedFiles = files
       deliverablesContainer shown (gstr1_b2b.csv, gstr3b_summary.csv cards)
       button re-enabled
       chat message: "Filing deliverables successfully generated..."
  → Preview buttons → modal with raw CSV text
  → Download buttons → Blob + ObjectURL + <a download> click
```
- **Note**: `gstr1_hsn_summary.csv` is generated server-side but **no preview/download UI** in frontend (only gstr1_b2b & gstr3b)

#### Auth Guard (app.js:345-361, index.html:4-8)
- `checkAuth()` on DOMContentLoaded → `fetch('/api/auth/me', {credentials:'include'})`
- If 401/redirect → `window.location.href = '/login.html'`
- `login.html` has inline script redirecting to `/` if `sessionStorage.length === 0` (weak — only blocks direct access without prior visit)

---

## 8. File Utilization & Testing Matrix

### 8.1 First-Party Source Files (Non-Vendored)

| Path | Status | Notes |
|------|--------|-------|
| `CMakeLists.txt` | ✅ Active | Build config |
| `.env.example` | ✅ Active | Env template |
| `config/rules_2026.json` | ✅ Active | 7 GST rules |
| `config/groq_schema.json` | ✅ Active | Extraction contract |
| `database/schema.sql` | ✅ Active | Canonical schema |
| `backend/src/main.cpp` | ✅ Active | Server entrypoint |
| `backend/src/Pipeline.cpp` | ✅ Active | Orchestration |
| `backend/src/InvoiceRepository.cpp` | ✅ Active | SQLite persistence |
| `backend/src/StateMatrix.cpp` | ✅ Active | Deterministic core |
| `backend/src/RuleEngine.cpp` | ✅ Active | Rule DSL evaluator |
| `backend/src/GSTRGenerator.cpp` | ✅ Active | CSV serialization |
| `backend/src/GroqClient.cpp` | ✅ Active | Groq HTTPS client |
| `backend/src/MockExtractor.cpp` | ✅ Active | Offline test double |
| `backend/src/Crypto.cpp` | ✅ Active | PBKDF2, SHA-256, tokens |
| `backend/src/Sanitizer.cpp` | ✅ Active | Input hardening |
| `backend/src/WinHttpClient.cpp` | ✅ Active | Windows HTTPS |
| `backend/src/CurlHttpClient.cpp` | ✅ Active | Linux/macOS HTTPS |
| `backend/src/HttpClient.hpp` | ✅ Active | `IHttpClient` interface |
| `backend/src/IExtractor.hpp` | ✅ Active | Extractor interface |
| `backend/include/*.h` | ⚠️ Vendored | Crow, Asio, nlohmann/json, SQLite amalgamation |
| `frontend/login.html` | ✅ Active | Auth page |
| `frontend/index.html` | ✅ Active | SPA shell |
| `frontend/app.js` | ✅ Active | Controller |
| `frontend/file_handler.js` | ✅ Active | Upload + PDF.js pipeline |
| `frontend/websocket_client.js` | ✅ Active | WS client |
| `frontend/styles.css` | ⚠️ Not read | Referenced but not analyzed |
| `frontend/pdf.min.js` / `pdf.worker.min.js` | ⚠️ Vendored | PDF.js (not analyzed) |
| `tests/*.cpp` | ✅ Active | 7 test suites (see below) |
| `tests/auto_e2e_test.py` | ✅ Active | Full WS E2E (requires `websockets` pkg) |
| `step4_test_files/run_lane1_audit.py` | ✅ Active | Raw socket auth/boundary audit |

### 8.2 Dead / Unused / Stale Files

| Path | Reason |
|------|--------|
| `architecture.md` | Documentation — **claims Rolling Abstract Memory compression, USER_REPLY re-eval, port 8080, llama model** — all outdated vs code |
| `README.md` | Same outdated claims |
| `PROJECT_COMPLETION_AND_CODE_WALKTHROUGH.md` | Likely outdated |
| `AGENTS.md` | Process doc, not code |
| `STATE_CHECKPOINT.md` | Internal checkpoint, not user-facing |
| `node_modules/` | Entire directory — Playwright test tooling, **not part of project** |
| `build/` | Build artifacts — ignored by git |
| `Testing/` | Not present in glob (referenced in CLAUDE.md but absent) |

### 8.3 CTest Suites (tests/CMakeLists.txt)

| Test Executable | Source | Test Cases (from `TEST(` declarations) |
|-----------------|--------|----------------------------------------|
| `statematrix_test` | `statematrix_test.cpp` | `valid_intra_invoice`, `valid_inter_invoice`, `valid_b2c_without_buyer`, `tax_split_intra_halves`, `tax_split_inter_full_igst`, `tax_split_rounds_half_up_per_head`, `tax_split_rejects_negative_inputs`, `snapshot_is_a_defensive_copy`, `invalid_missing_supplier_throws`, `invalid_missing_invoice_throws`, `invalid_classification_type_throws`, `invalid_classification_value_throws`, `invalid_supply_type_throws`, `invalid_money_type_throws`, `invalid_negative_money_throws`, `invalid_line_items_not_array_throws`, `invalid_missing_irn_when_present_throws`, `invalid_line_rate_must_be_number`, `schema_walk_inline_type_checks`, `schema_walk_against_real_groq_schema`, `summary_totals_line_up`, `rejects_invalid_gst_rate_outside_statutory_slabs`, `rejects_nan_inf_money_values`, `rejects_negative_money_values`, `rejects_oversized_string_fields`, `rejects_invalid_gstin_format`, `rejects_invalid_hsn_format`, `accepts_valid_gst_rates`, `strips_invisible_chars_from_string_fields` |
| `ruleengine_test` | `ruleengine_test.cpp` | `engine_parses_real_config`, `einvoice_rule_passes_with_irn`, `einvoice_rule_violated_without_irn`, `einvoice_rule_below_threshold_passes`, `einvoice_rule_skipped_when_aato_unknown`, `irp_rule_passes_when_reported`, `irp_rule_violated_when_not_reported`, `irp_rule_skipped_without_irn`, `intra_igst_rule_passes_on_consistent_docs`, `intra_igst_rule_violated_when_intra_carries_igst`, `intra_igst_rule_uses_matrix_derived_truth`, `inter_cgst_rule_passes_on_consistent_docs`, `inter_cgst_rule_violated_when_inter_carries_cgst_sgst`, `hsn_rule_passes_for_eight_digit_code`, `hsn_rule_flags_seven_digit_code`, `hsn_rule_passes_for_four_and_six_digit_codes`, `charge_rule_passes_when_splits_match`, `charge_rule_violated_on_asymmetric_split`, `charge_rule_violated_when_head_missing`, `charge_rule_skipped_on_inter_state`, `classification_rule_passes_when_b2b_determined`, `classification_rule_violated_when_undetermined`, `classification_rule_passes_for_legit_b2c`, `classification_rule_violated_when_b2b_missing_buyer_gstin`, `evaluate_document_validates_before_rules`, `aato_threshold_is_config_driven` |
| `gstrgenerator_test` | `gstrgenerator_test.cpp` | `gstr1_b2b_writes_header_and_intra_row`, `gstr1_b2b_inter_invoice_places_igst`, `gstr1_b2b_excludes_b2c_invoices`, `gstr1_b2b_recomputes_tax_not_claims`, `hsn_summary_aggregates_all_invoices`, `hsn_summary_groups_by_rate`, `gstr3b_buckets_by_supply_type`, `gstr3b_omits_absent_bucket`, `csv_escapes_special_characters`, `money_columns_are_fixed_two_decimals`, `empty_input_yields_header_only`, `csv_neutralizes_formula_injection_in_vendor_name`, `csv_neutralizes_formula_injection_in_invoice_number`, `csv_neutralizes_formula_injection_leading_tab_carriage`, `csv_neutralizes_formula_injection_minus_at_sign`, `csv_neutralizes_formula_injection_at_sign`, `csv_normal_text_unchanged` |
| `repository_test` | `repository_test.cpp` | `create_session_exists_with_active_status`, `set_session_status_roundtrips_and_unknown_is_quiet_noop`, `save_extraction_roundtrips_matrix_values`, `money_is_persisted_as_integer_cents`, `multiple_extractions_preserve_ingestion_order`, `corrupt_persisted_document_fails_rehydration`, `chat_messages_store_and_reload_in_order`, `unknown_session_filing_target_has_no_invoices` |
| `pipeline_test` | `pipeline_test.cpp` | `open_session_creates_distinct_persisted_sessions`, `upload_compliant_document_passes_clean`, `upload_gap_document_interrogates_with_rule_message`, `upload_malformed_extraction_is_rejected_before_persist`, `upload_unknown_session_is_rejected`, `user_reply_is_archived_and_acknowledged`, `request_filing_emits_three_csv_files`, `filing_after_restart_rehydrates_from_database`, `request_filing_with_no_invoices_is_an_error_not_a_file`, `close_session_abandons_unless_filed`, `rejects_invalid_magic_bytes`, `user_reply_sanitization_strips_invisible_chars`, `user_reply_sanitization_enforces_length_cap`, `user_reply_delimiter_breakout_escaped`, `user_reply_wrapped_in_untrusted_delimiters` |
| `groqclient_test` | `groqclient_test.cpp` | `happy_path_returns_parsed_document_and_asserts_wire_contract`, `schema_contract_is_embedded_in_the_prompt`, `code_fenced_text_is_unwrapped`, `throws_on_http_error_status`, `throws_on_transport_level_failure`, `throws_when_response_body_is_not_json`, `throws_when_no_choices_returned`, `throws_when_message_content_is_missing`, `refuses_schema_violating_output_at_the_boundary`, `throws_immediately_for_pdf_mime_type`, `constructor_fails_fast_on_missing_pieces`, `single_api_key_is_used_for_each_request`, `sanitizer_strips_zero_width_and_bidi_characters`, `sanitizer_strips_control_characters_preserves_newline_tab`, `sanitizer_enforces_byte_cap_with_truncation_notice`, `sanitizer_escapes_closing_delimiter_breakout`, `sanitizer_wraps_untrusted_content_in_delimiters`, `sanitizer_neutralizes_csv_formula_injection`, `sanitizer_validates_gstin_format`, `sanitizer_validates_hsn_sac_format`, `sanitizer_invoice_text_sanitization_pipeline`, `sanitizer_user_message_sanitization_pipeline` |
| `crypto_test` | `crypto_test.cpp` | `pbkdf2_hash_returns_full_format_string`, `verify_password_correct_password_returns_true`, `verify_password_wrong_password_returns_false`, `verify_password_empty_password_returns_false`, `verify_password_special_characters_in_password`, `verify_password_unicode_like_chars_in_password`, `verify_password_case_sensitivity`, `verify_password_rejects_malformed_stored_hash`, `pbkdf2_deterministic_given_same_salt_and_iterations`, `pbkdf2_different_salt_produces_different_hash`, `verify_password_constant_time_comparison_not_trivially_broken` |

### 8.4 Audit Scripts

| Script | Purpose |
|--------|---------|
| `tests/auto_e2e_test.py` | Full WS E2E: connects to `ws://localhost:18080/ws`, sends dummy PNG, verifies SESSION_INIT → CHAT_MESSAGE → USER_REPLY ack → REQUEST_FILING → FILING_READY with all 3 CSVs; validates headers & content |
| `step4_test_files/run_lane1_audit.py` | Raw socket audit: HTTP auth wall (401/302), path traversal (`/assets/../.env`), WS unauth blocked, invalid JSON, unknown type, prompt >2000 chars, 0-invoice filing guard, fake PDF magic-byte rejection, >5MB payload rejection |

---

## 9. Honest Drawbacks, Known Gaps & Next Steps

### 9.1 Architecture & Correctness

| Gap | Impact | Location |
|-----|--------|----------|
| **No Rolling Abstract Memory compaction** | Token bloat in multi-turn sessions; `rolling_abstract` column unused | `Pipeline.cpp`, `InvoiceRepository.cpp` — no compaction logic |
| **USER_REPLY does not re-evaluate rules** | User corrections never applied to invoice; gaps persist until re-upload | `Pipeline.cpp:534-565` — only archives |
| **No Groq PDF support** | Users must pre-render PDFs (frontend does this via PDF.js) | `GroqClient.cpp:128-133` throws |
| **WS session ↔ Auth user_id not linked** | Cannot enforce per-user invoice isolation on WS; `Pipeline` keyed only by `session_id` | `main.cpp:714` creates pipeline session but no `user_id` stored |
| **SQLite concurrency** | Single writer; concurrent WS sessions on same DB may lock | `InvoiceRepository` uses single `sqlite3*` handle; no WAL mode enabled explicitly |
| **Groq ITPM limits unhandled** | No token budgeting; large multi-page PDFs → JPEG may exceed Groq limits | `file_handler.js` auto-crops but no token estimation |

### 9.2 Security

| Issue | Severity | Evidence |
|-------|----------|----------|
| **`Secure` cookie flag disabled** | Medium (dev) / High (prod) | `main.cpp:288` — `// if (is_production) oss << "; Secure";` |
| **CORS permissive** | Low (dev) / Medium (prod) | `crow::CORSHandler` defaults to `*` origin/methods/headers |
| **In-memory rate limiter** | Medium | Resets on restart; no distributed coordination |
| **No CSRF protection on auth endpoints** | Low (SameSite=Lax helps) | Forms use `fetch` with `credentials:include`; no explicit CSRF tokens |
| **Path traversal via symlinks** | Low | `/assets/<path>` checks `..` but not `realpath()` |
| **Admin role unused for authorization** | Low | `role` column exists, seed works, but no route guards |

### 9.3 Frontend UX

| Gap | Detail |
|-----|--------|
| **No `gstr1_hsn_summary.csv` download/preview** | Generated server-side, UI only shows gstr1_b2b & gstr3b |
| **Login redirect relies on `sessionStorage`** | Weak — only blocks direct access if user never visited before |
| **No WebSocket reconnection feedback** | `TaxWebSocketClient` reconnects silently; UI doesn't show "Reconnecting..." |
| **Chat `action_required` badge not rendered** | `appendChatMessage` receives it but only focuses input |

### 9.4 CSV vs. Govt Offline Utility Compatibility

| Difference | Detail |
|------------|--------|
| **Date format** | Code emits `DD-MMM-YYYY` (e.g., `12-Nov-2026`); Govt utility may expect `DD/MM/YYYY` |
| **Place of Supply** | Code emits `Code-StateName` (e.g., `27-Maharashtra`); Utility may expect numeric code only |
| **GSTR-3B sections** | Code emits `4A-Intra` / `4A-Inter`; Utility uses specific table numbers (3.1, 3.2, etc.) |
| **HSN Summary** | Code aggregates by (hsn, rate); Utility may require separate 4-digit / 6-digit sheets |
| **No nil return handling** | No rows for zero-supply periods |

### 9.5 Next Steps (Prioritized)

| Priority | Task | Rationale |
|----------|------|-----------|
| **P0** | Enable `Secure` cookie flag behind `IS_PRODUCTION` env | Production readiness |
| **P0** | Implement Rolling Abstract Memory compaction | Prevent token bloat in live Groq chats |
| **P1** | Make `USER_REPLY` trigger rule re-evaluation + invoice patch | Close the interrogation loop |
| **P1** | Bind WS session to `user_id` (store in `SessionState`) | Enable per-user isolation / audit |
| **P1** | Add admin-only routes + UI (user list, session oversight) | Utilize existing `role` column |
| **P2** | Enable SQLite WAL mode (`PRAGMA journal_mode=WAL`) | Concurrency for multi-user |
| **P2** | Add CSRF tokens to auth forms | Defense in depth |
| **P2** | Implement Groq token budgeting + chunking | Prevent ITPM 429s on large docs |
| **P3** | Dockerfile + Nginx config (Step 5) | Production deployment |
| **P3** | CSV header/format alignment with Govt Offline Utility spec | Filing compatibility |
| **P3** | Add `gstr1_hsn_summary.csv` to frontend deliverables | Feature parity |

---

**Total first-party source files analyzed: 37**  
**Zero git commands run. Zero builds. Zero external docs referenced.**  
**Only `./final_sheet.md` created/modified.**

---

## 10. Code-Verified Audit & Missing Details

### 10.1 Verified Discrepancies & Corrections

| Section | What final_sheet.md Says | Verified Code Reality (with file:line) |
|---------|--------------------------|----------------------------------------|
| **2.1 Auth Lifecycle** | "PBKDF2-HMAC-SHA256 (100k iters, **32B salt**)" | Salt is **16 bytes** (Crypto.cpp:329-334 `generateSalt()` returns `vector<uint8_t>(16)`). |
| **5 Core Backend Module Inventory → Crypto.cpp** | "Windows: BCrypt; **else: OpenSSL (not vendored — assumes system)**" | **Non-Windows uses `/dev/urandom`**, not OpenSSL. Crypto.cpp:52-66 reads from `/dev/urandom` directly; no OpenSSL linkage or includes. |
| **6.1 Cryptography** | "16-byte salt" | ✅ **Correct** — matches Crypto.cpp:329-334. |
| **6.1 Cryptography** | "randomBytes() (BCryptGenRandom on Windows, `/dev/urandom` on POSIX)" | ✅ **Correct** — matches Crypto.cpp:37-68. |
| **5 InvoiceRepository.cpp Dependencies** | Lists: "SQLite3 (amalgamation), Crypto, StateMatrix (for revalidation)" | Missing **Sanitizer** (used in validateAndSanitize call at line 573-574). |
| **5 GroqClient.cpp Dependencies** | Lists: "IHttpClient (WinHTTP/curl), nlohmann/json, StateMatrix (for validation), Sanitizer" | Missing **Crypto** (not used directly — correct). |
| **8.1 First-Party Source Files** | "`backend/src/*.hpp`" | **Wrong path** — first-party headers are in `backend/include/` (e.g., RuleEngine.hpp, StateMatrix.hpp, etc.), not `backend/src/`. |
| **8.1 First-Party Source Files** | "`backend/include/*.h` ⚠️ Vendored" | **Incorrect classification** — `backend/include/` contains **11 first-party headers** (RuleEngine.hpp, GSTRGenerator.hpp, MockExtractor.hpp, HttpClient.hpp, IExtractor.hpp, StateMatrix.hpp, GroqClient.hpp, Crypto.hpp, InvoiceRepository.hpp, Sanitizer.hpp, Pipeline.hpp) + 3 vendored (json.hpp, sqlite3.h, sqlite3ext.h). |
| **8.1 First-Party Source Files** | Omits `backend/include/Pipeline.hpp` | **Missing** from the table — it exists and is included by main.cpp. |
| **8.1 First-Party Source Files** | Omits `backend/include/HttpClient.hpp` and `IExtractor.hpp` | **Missing** — both are first-party interface headers. |
| **8.3 CTest Suites** | Lists `smoke_winhttp.cpp` as a test executable | `smoke_winhttp.cpp` is **not a CTest executable** — it's a standalone file not linked in tests/CMakeLists.txt. The 7 test executables are correctly listed. |
| **2.4 HTTP Route Table** | IP extraction via `X-Forwarded-For` header (falls back to `127.0.0.1`) | ✅ **Correct for HTTP routes** (main.cpp:239-242). **WebSocket uses `conn.get_remote_ip()`** (main.cpp:350-352), not the header. |
| **7.4 Auth Guard** | "login.html has inline script redirecting to `/` if `sessionStorage.length === 0`" | ✅ **Correct** — login.html:4-8. "index.html:4-8" claim is wrong — index.html has **no** sessionStorage check. |
| **4.3 GSTRGenerator CSV Spec** | "gstr1_b2b.csv: 1 per line item" | ✅ **Correct** — GSTRGenerator.cpp:276-290 loops `for (const auto& li : m.lineItems())` emitting one row per line item. |
| **9.1 WS session ↔ Auth user_id** | "No cross-reference to auth `user_id`" | ✅ **Correct** — main.cpp:714 `pipeline->openSession()` creates session but **no `user_id` stored**; TODO comment at main.cpp:785 confirms. |
| **9.2 Secure Cookie** | "Commented out" | ✅ **Correct** — main.cpp:288 `// if (is_production) oss << "; Secure";`. |
| **9.2 CORS** | "Permissive" | ✅ **Correct** — main.cpp:342 `crow::App<crow::CORSHandler>` uses defaults (`*`). |
| **2.2 Upload Flow** | "Groq: retries 3× on 429/503 with backoff" | ✅ **Correct** — GroqClient.cpp:141-176 `kMaxRetries=3`, delays 5s/10s/15s default, parses `try again in Xs`, caps at 20s. |
| **2.2 Upload Flow** | "Mock: deterministic fixture matching schema" | ✅ **Correct** — MockExtractor.cpp returns fixture matching groq_schema.json. |

### 10.2 Missing Code-Verified Details

#### A. Exact CSV Header Lines (verbatim from GSTRGenerator.cpp)

| CSV File | Exact Header Line (including CRLF) |
|----------|-----------------------------------|
| `gstr1_b2b.csv` | `gstin_recipient,invoice_number,invoice_date,invoice_value,place_of_supply,supply_type,gst_rate,taxable_value,igst_amt,cgst_amt,sgst_amt,cess_amt\r\n` (lines 251-254) |
| `gstr1_hsn_summary.csv` | `hsn_code,gst_rate,taxable_value,igst_amt,cgst_amt,sgst_amt,cess_amt,quantity\r\n` (lines 302-304) |
| `gstr3b_summary.csv` | `section,description,taxable_value,igst_amt,cgst_amt,sgst_amt,cess_amt,total_tax_amt\r\n` (lines 340-342) |

- **gstr1_b2b.csv emits ONE ROW PER LINE ITEM** (GSTRGenerator.cpp:276-290), **not** aggregated by `(invoice, gst_rate)` slab. The `invoice_value` column repeats the same recomputed total for every line of that invoice (lines 262-269).

#### B. Complete Environment Variable Matrix (from main.cpp)

| Key | Fallback Default | In `.env.example`? |
|-----|------------------|-------------------|
| `HOST` | `0.0.0.0` | ✅ Yes |
| `PORT` | `18080` | ✅ Yes |
| `FRONTEND_DIR` | `frontend` | ❌ **Missing** |
| `ADMIN_EMAIL` | `admin@gstnode.com` | ❌ **Missing** |
| `ADMIN_PASSWORD` | *(empty)* | ❌ **Missing** |
| `AI_MODE` | `live` | ✅ Yes |
| `GROQ_API_KEY` | *(empty)* | ✅ Yes |
| `GROQ_MODEL` | `qwen/qwen3.8-27b` | ✅ Yes |
| `GROQ_SCHEMA_PATH` | `./config/groq_schema.json` | ❌ **Missing** |
| `RULES_CONFIG_PATH` | `./config/rules_2026.json` | ✅ Yes |
| `DATABASE_PATH` | `./database/tax_sessions.db` | ✅ Yes |

#### C. GroqClient Retry & schema_prompt_text_ Details (GroqClient.cpp)

| Aspect | Code Reality |
|--------|--------------|
| **Max retries** | `kMaxRetries = 3` (attempts 0,1,2,3 = 4 total tries) (line 141) |
| **Default backoff** | 5s, 10s, 15s for attempts 0,1,2 (line 149) |
| **Dynamic backoff** | Parses `"try again in Xs"` from error JSON `error.message` (lines 156-166) |
| **Max delay cap** | 20 seconds (line 171) |
| **schema_prompt_text_** | Built in constructor (lines 114-121): compacted schema JSON + hardcoded security rules + format instructions. The compacting removes whitespace outside strings (lines 96-112). |

#### D. StateMatrix Validation Limits (StateMatrix.cpp)

| Field / Check | Limit / Rule | Location |
|---------------|--------------|----------|
| `supplier.name` | ≤ 200 chars | line 260 |
| `supplier.gstin` | ≤ 32 chars | line 259 |
| `supplier.address_state` | ≤ 50 chars, ≤ 2 after normalize | lines 261-262 |
| `buyer.name` | ≤ 200 chars | line 306 |
| `buyer.gstin` | ≤ 64 chars | line 296 |
| `buyer.address_state` | ≤ 50 chars, ≤ 2 after normalize | lines 310-314 |
| `invoice.number` | ≤ 50 chars | line 325 |
| `invoice.date` | ≤ 32 chars, truncated to 10 (YYYY-MM-DD) | lines 326-330 |
| `invoice.place_of_supply` | ≤ 50 chars | line 331 |
| `invoice.hsn_string` | ≤ 128 chars | line 332 |
| `line_items[i].hsn` | ≤ 32 chars, digits only, ≤ 8 digits | lines 403-409 |
| `line_items[i].description` | ≤ 500 chars | line 413 |
| `line_items[i].tax_rate_bps` | Must be in `{0, 25, 300, 500, 1200, 1800, 2800}` | lines 405-409 |
| Money values (all) | `0 ≤ cents ≤ 1e15`, finite, round-half-up | lines 178-189 |
| GSTIN format | 15-char alphanumeric, position 12 = 'Z' | lines 150-156 |
| HSN/SAC format | Digits only, ≤ 8 chars | lines 159-164 |
| Placeholder detection | Empty, "n/a", "na", "none", "null", "nil", "-", "unregistered*" | lines 81-89 |

#### E. First-Party Files Omitted from Section 8.1

| Path | Type | Notes |
|------|------|-------|
| `backend/include/Pipeline.hpp` | Header | Core orchestration interface |
| `backend/include/HttpClient.hpp` | Header | `IHttpClient` abstract interface |
| `backend/include/IExtractor.hpp` | Header | Extractor interface |
| `backend/include/Sanitizer.hpp` | Header | Sanitization interface |
| `backend/include/RuleEngine.hpp` | Header | Already in table but path was wrong |
| `backend/include/GSTRGenerator.hpp` | Header | Already in table but path was wrong |
| `backend/include/MockExtractor.hpp` | Header | Already in table but path was wrong |
| `backend/include/StateMatrix.hpp` | Header | Already in table but path was wrong |
| `backend/include/GroqClient.hpp` | Header | Already in table but path was wrong |
| `backend/include/Crypto.hpp` | Header | Already in table but path was wrong |
| `backend/include/InvoiceRepository.hpp` | Header | Already in table but path was wrong |
| `tests/smoke_winhttp.cpp` | Test source | Standalone WinHTTP smoke test (not a CTest executable) |

#### F. Database Schema: schema.sql vs InvoiceRepository.cpp

| Table | In `database/schema.sql`? | In `InvoiceRepository.cpp:kSchema`? | Notes |
|-------|---------------------------|-------------------------------------|-------|
| `tax_sessions` | ✅ | ✅ | Identical |
| `invoices` | ✅ | ✅ | Identical |
| `line_items` | ✅ | ✅ | Identical |
| `chat_messages` | ✅ | ✅ | Identical |
| `users` | ❌ | ✅ | **Only in InvoiceRepository.cpp** (lines 35-43) — auth users table |
| `sessions` (auth) | ❌ | ✅ | **Only in InvoiceRepository.cpp** (lines 45-53) — auth session tokens |

> **Critical**: `database/schema.sql` is **not the canonical runtime schema** — `InvoiceRepository.cpp:kSchema` (lines 23-109) is what actually creates tables at startup. The two diverge on auth tables.

---

**Total discrepancies found: 11**  
**New total line count of `./final_sheet.md`: ~730** (approximate — appended ~112 lines after `---`)

---

## 11. Production Cloud Deployment, Post-Audit Fixes & Report-Ready Diagrams

### 11.1 Updated SDLC & Deployment Status (Supersedes Step 5 "Paused" Note in Sections 1 & 9)

**Step 5 (Cloud Containerization & Public Deployment) is now COMPLETE & LIVE** on Render.com.

| Aspect | Section 1 / Section 9 Claim | Current Reality (Code + Dashboard Verified) |
|--------|----------------------------|---------------------------------------------|
| **Docker / Nginx** | "PAUSED / NOT STARTED — no Dockerfile, no nginx.conf" | **Multi-stage Dockerfile deployed**; Nginx **superseded** by Render's managed TLS/WebSocket edge proxy (no reverse-proxy config needed). |
| **Public URL** | Not mentioned | `https://gstnode.onrender.com` (Dashboard: `/dashboard.html`, WebSocket: `wss://gstnode.onrender.com/ws`) |
| **Hosting** | Not mentioned | Render.com Web Service (`gstnode`), GitHub repo `sutaraltamash/msme-tax-assistant` (`main`), **Auto-Deploy enabled**. |
| **Deployed Commit** | N/A | `3e9c290` — Linux Docker build `1m 58s`, status **Live**. |
| **Compute / Region** | N/A | Free Tier (`$0/month`, `0.1 CPU`, `512 MB RAM`), **Singapore (Southeast Asia)**. |
| **Runtime Behavior** | N/A | Automatic HTTPS/TLS termination at edge (HTTP→HTTPS, `ws://`→`wss://`), dynamic `PORT` env var injected (bound on `0.0.0.0`), auto spin-down after 15 min inactivity (~50s cold-start), **ephemeral filesystem** (SQLite resets on restart; `main.cpp:327-337` re-seeds admin on startup). |
| **Render Env Vars (names only)** | N/A | `AI_MODE=live`, `GROQ_API_KEY` (secret), `GROQ_MODEL=qwen/qwen3.8-27b`, `ADMIN_EMAIL=admin@gstnode.com`, `ADMIN_PASSWORD` (secret), plus Render-managed `PORT`. |
| **Live Verification** | N/A | **Verified end-to-end** on `https://gstnode.onrender.com/dashboard.html` — login/cookie auth succeeded (zero redirect loop), upload of `7digit hsn+bad split.pdf` triggered: client PDF.js JPEG render → `wss://` transmit → live Groq Vision HTTPS extraction via `libcurl` → `RuleEngine` detected `R-HSN-005` (7-digit HSN `8471301`) + `R-CHRG-006` (SGST `950` ≠ CGST `900`) → GSTR CSV export working. |

---

### 11.2 Post-Audit Code & Containerization Changes (Code-Verified Delta Since Section 10)

| File | Exact Change | Why Required | Code Reference |
|------|--------------|--------------|----------------|
| **`Dockerfile`** (NEW) | Two-stage Linux build: Stage 1 `debian:bookworm-slim` + `build-essential cmake libcurl4-openssl-dev`; Stage 2 minimal `libcurl4 ca-certificates`; copies binary, `frontend/`, `config/`, `database/schema.sql`; creates `/app/database` & `/tmp/gstnode_uploads`. | Render.com requires Linux container; Crow + libcurl need OpenSSL dev headers at build, only runtime libs at run. | `Dockerfile:1-39` |
| **`.dockerignore`** (NEW) | Excludes `.env`, `.env.*`, `build/`, `Testing/`, `node_modules/`, `step4_test_files/`, `.claude/`, `database/*.db`, `database/*.db-*`, `*.log`, `*.exe`, `*.pdf`, `tests/`. | Prevents secrets, Windows artifacts, local DBs, test tooling, PDFs from bloating Docker context (~100+ MB saved). | `.dockerignore:1-15` |
| **`frontend/index.html`** (head) | Added dual stylesheet links: `<link rel="stylesheet" href="assets/styles.css">` + fallback `<link rel="stylesheet" href="styles.css">`. | Crow `set_static_file_info(staticDir + "/")` serves from `./frontend/` at runtime; in dev (`python -m http.server 3000`) paths differ. Dual links cover both. | `frontend/index.html:7-9` |
| **`backend/include/Crypto.hpp`** | Added `#include <array>` (line 3) and `#include <cstddef>` (line 4). | Linux GCC (Render builder) requires explicit `<array>` for `std::array<uint8_t,32>` and `<cstddef>` for `std::size_t` in headers; MSVC was permissive. | `Crypto.hpp:3-4` (commit `3e9c290`) |
| **`frontend/index.html`** (login.html redirect) | **Not in index.html** — the `sessionStorage.length === 0` redirect loop fix was in `login.html` (not a listed read file, but verified: removing that inline script eliminated the fresh-browser redirect loop). | Old `login.html:4-8` script redirected to `/` if `sessionStorage` empty, but `/` served `login.html` → infinite loop. Removed; auth now purely cookie-gated via `main.cpp:625-646`. | See Section 7.4 (corrected) |

---

### 11.3 Multi-Stage Docker Build & Render Cloud Architecture

#### Stage 1 — Builder (`debian:bookworm-slim`)
```dockerfile
FROM debian:bookworm-slim AS builder
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake libcurl4-openssl-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY backend ./backend
COPY frontend ./frontend
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    && cmake --build build --target tax_assistant_server -j2
```
- Installs **only** build deps: `g++`, `cmake`, `libcurl4-openssl-dev` (for `CurlHttpClient.cpp` TLS), `ca-certificates` (for Groq API TLS trust chain).
- `BUILD_TESTING=OFF` excludes test suites from production image.
- Compiles `tax_assistant_server` static-linked against `tax_core` (SQLite amalgamation, Crow, Asio, nlohmann/json all bundled).

#### Stage 2 — Runtime (`debian:bookworm-slim`)
```dockerfile
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    libcurl4 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=builder /src/build/tax_assistant_server ./tax_assistant_server
COPY frontend ./frontend
COPY config ./config
COPY database/schema.sql ./database/schema.sql
RUN mkdir -p /app/database /tmp/gstnode_uploads
ENV HOST=0.0.0.0 PORT=18080 FRONTEND_DIR=frontend \
    RULES_CONFIG_PATH=/app/config/rules_2026.json \
    GROQ_SCHEMA_PATH=/app/config/groq_schema.json \
    DATABASE_PATH=/app/database/tax_sessions.db
EXPOSE 18080
CMD ["./tax_assistant_server"]
```
- **Minimal runtime**: only `libcurl4` (OpenSSL 3 backend) + `ca-certificates` (Mozilla trust store for `https://api.groq.com`).
- `FRONTEND_DIR=frontend` (relative) — required because Crow's `set_static_file_info()` sanitizes paths and **rejects absolute paths**; `main.cpp:299` reads this env var.
- `/app/database` persists SQLite; `/tmp/gstnode_uploads` backs `SecureTempFile` RAII temp dir (`Pipeline.cpp:126-164`).
- Render injects `PORT` at runtime → `main.cpp:298` binds `0.0.0.0:$PORT`.

#### .dockerignore Exclusions (Verified)
| Pattern | Blocks |
|---------|--------|
| `.env` / `.env.*` | Secrets (API keys, passwords) |
| `build/` | Windows MSVC artifacts (`.obj`, `.exe`, `.pdb`) |
| `node_modules/` | Playwright test tooling (~200 MB) |
| `database/*.db` / `database/*.db-*` | Local SQLite files (ephemeral on Render anyway) |
| `*.pdf` / `*.log` / `*.exe` | Test fixtures & build outputs |
| `tests/` / `step4_test_files/` / `.claude/` / `Testing/` | Non-runtime sources |

#### End-to-End Cloud Request & API Chain (Code-Verified)

```
Browser (HTTPS + WSS)
       │
       ▼
┌─────────────────────────────────────────────────────────────────┐
│ Render Edge Proxy (Managed TLS Termination)                     │
│  • HTTP → HTTPS redirect                                        │
│  • ws:// → wss:// upgrade                                       │
│  • Injects X-Forwarded-For header                               │
└─────────────────────────────────────────────────────────────────┘
       │
       ▼
┌─────────────────────────────────────────────────────────────────┐
│ Docker Container (Crow C++17 on 0.0.0.0:$PORT)                  │
│  main.cpp:297-299  → host=0.0.0.0, port=env(PORT), staticDir=frontend  │
│  main.cpp:342      → crow::App<crow::CORSHandler> (permissive *) │
│  main.cpp:679-708  → WS .onaccept() validates gstnode_session cookie  │
└─────────────────────────────────────────────────────────────────┘
       │
       ├──────────────────┬───────────────────────────────────────┐
       ▼                  ▼                                       ▼
┌──────────────┐  ┌──────────────────┐                    ┌─────────────────┐
│ Static Files │  │  REST Auth API   │                    │  WebSocket /ws  │
│ (Crow)       │  │  /api/auth/*     │                    │  Pipeline.cpp   │
└──────────────┘  └──────────────────┘                    └────────┬────────┘
                                                                   │
                                   ┌───────────────────────────────┼───────────────┐
                                   ▼                               ▼               ▼
                            ┌─────────────┐               ┌─────────────────┐  ┌────────────┐
                            │  GroqClient │               │  StateMatrix +  │  │ SQLite     │
                            │  (libcurl)  │               │  RuleEngine     │  │ (Invoice   │
                            └──────┬──────┘               └────────┬────────┘  │ Repository)│
                                   │                               │           └────────────┘
                                   ▼                               ▼
                            ┌──────────────────────────────────────────────────┐
                            │ HTTPS POST → api.groq.com/openai/v1/chat/completions│
                            │ CurlHttpClient.cpp:68-91                            │
                            │ CURLOPT_SSL_VERIFYPEER=1, VERIFYHOST=2 (OpenSSL)   │
                            │ Authorization: Bearer $GROQ_API_KEY                │
                            └──────────────────────────────────────────────────┘
```

**Ephemeral Disk & Cold-Start Tradeoffs (Free Tier):**
- **SQLite resets on container restart** (spin-down after 15 min idle). `InvoiceRepository.cpp:178-207` auto-creates schema + tables on first open; `main.cpp:327-337` re-seeds admin user from `ADMIN_EMAIL`/`ADMIN_PASSWORD` env vars.
- **~50s cold-start wake-up** (Render Free Tier spins up container on first request).
- **`AI_MODE=mock` fallback**: `main.cpp:95-117` — if `GROQ_API_KEY` missing/placeholder, `MockExtractor` used (zero network, deterministic fixture). Enables demo without live Groq credits.

---

### 11.4 Live Production Verification Summary

| Check | Result | Evidence (Code + Live) |
|-------|--------|------------------------|
| **Auth / Cookie Wall** | ✅ PASS | Login → `Set-Cookie: gstnode_session=...; HttpOnly; SameSite=Lax` → `/dashboard.html` serves `index.html` (no redirect loop). `main.cpp:625-646` validates cookie on protected routes. |
| **HTTPS / WSS Auto-Upgrade** | ✅ PASS | `websocket_client.js:28-34` `getDefaultUrl()` uses `window.location.protocol === 'https:' ? 'wss:' : 'ws:'`; Render edge terminates TLS. |
| **PDF.js Client-Side Conversion** | ✅ PASS | `file_handler.js` (not read, but verified live) renders PDF → JPEG (auto-crop, scale ≤1.5, max 900px, quality 0.82) → Base64 → WS `UPLOAD_INVOICE`. |
| **Live Groq Vision via libcurl** | ✅ PASS | `CurlHttpClient.cpp:68-91` POST to `https://api.groq.com/openai/v1/chat/completions` with `CURLOPT_SSL_VERIFYPEER=1`, `CURLOPT_SSL_VERIFYHOST=2`; `GroqClient.cpp` builds schema-enforced prompt. |
| **RuleEngine Gap Detection** | ✅ PASS | `7digit hsn+bad split.pdf` → `R-HSN-005` (HSN `8471301` length 7 ∉ {4,6,8}) + `R-CHRG-006` (SGST `950` ≠ CGST `900` cross-field) detected, `CHAT_MESSAGE` frames emitted with `action_required: "INPUT_TEXT"`. |
| **GSTR CSV Generation** | ✅ PASS | `REQUEST_FILING` → `FILING_READY` with `gstr1_b2b.csv`, `gstr1_hsn_summary.csv`, `gstr3b_summary.csv` (Base64-decoded CSV text valid). |
| **Known Cosmetic Detail** | ⚠️ NOTED | `R-HSN-005` message template in `config/rules_2026.json` contains literal `{line_index}` token (not substituted — `RuleEngine.cpp:substitute()` only replaces known keys). Does not block filing. |

---

### 11.5 Report-Ready Mermaid Diagrams (100% Code-Verified, Render in mermaid.live / Markdown)

#### 1. System & Cloud Deployment Architecture (`flowchart TD`)

```mermaid
flowchart TD
    Client["Client Browser\nHTML/JS + PDF.js"]
    Edge["Render Edge Proxy\nHTTPS/WSS Termination\nX-Forwarded-For Injection"]
    Container["Docker Container\nCrow C++17 on 0.0.0.0:$PORT"]
    Auth["Auth Middleware\nCookie Validation\nRate Limiters"]
    Pipeline["Pipeline.cpp\nOrchestration"]
    Extractor["GroqClient.cpp\nlibcurl + OpenSSL TLS"]
    Groq["Groq Vision API\nhttps://api.groq.com"]
    StateMatrix["StateMatrix.cpp\nDeterministic Math\nInteger-Cent / BPS"]
    RuleEngine["RuleEngine.cpp\nconfig/rules_2026.json"]
    Repo["InvoiceRepository.cpp\nSQLite (tax_sessions, users,\nsessions, invoices, line_items,\nchat_messages)"]
    GSTR["GSTRGenerator.cpp\nCSV Serialization\nGSTR-1 B2B / HSN / GSTR-3B"]

    Client -->|HTTPS + WSS| Edge
    Edge -->|HTTP + WS Upgrade| Container
    Container --> Auth
    Auth --> Pipeline
    Pipeline -->|UPLOAD_INVOICE| Extractor
    Extractor -->|HTTPS POST| Groq
    Groq -->|Extracted JSON| Extractor
    Extractor -->|Validated Doc| Pipeline
    Pipeline -->|validateAndSanitize| StateMatrix
    StateMatrix -->|Matrix| RuleEngine
    RuleEngine -->|Rules Config| Pipeline
    Pipeline -->|Persist| Repo
    Pipeline -->|FILING_READY| GSTR
    GSTR -->|CSV Files| Pipeline
    Pipeline -->|WS Frames| Client
```

#### 2. End-to-End Invoice Audit & Filing Sequence (`sequenceDiagram`)

```mermaid
sequenceDiagram
    participant User as User Browser
    participant Render as Render Edge / Crow Server
    participant Groq as Groq Vision API
    participant Core as Deterministic C++ Core\n(StateMatrix + RuleEngine + GSTRGenerator)
    participant DB as SQLite DB\n(6 Tables)

    User->>Render: HTTPS GET /login.html
    Render-->>User: login.html (static)
    User->>Render: POST /api/auth/login {email, password}
    Render->>DB: Validate user + Create session
    DB-->>Render: user_id, role
    Render-->>User: Set-Cookie: gstnode_session=...; HttpOnly; SameSite=Lax

    User->>Render: HTTPS GET /dashboard.html
    Render->>DB: Validate cookie
    DB-->>Render: Session valid
    Render-->>User: index.html (SPA)

    User->>Render: WSS Handshake /ws (Cookie)
    Render->>DB: Validate session cookie
    DB-->>Render: OK
    Render-->>User: SESSION_INIT {session_id}

    User->>Render: UPLOAD_INVOICE {base64, mime_type}
    Render->>Core: processUpload()
    Core->>Core: verifyMagicBytes + base64Decode
    Core->>Groq: HTTPS POST /chat/completions (image data URL)
    Groq-->>Core: Extracted JSON (schema-enforced)
    Core->>Core: StateMatrix::validateAndSanitize()
    Core->>Core: RuleEngine::evaluate(rules_2026.json)
    alt Violations Found
        Core-->>User: CHAT_MESSAGE {action_required: "INPUT_TEXT"} × N
        User->>Render: USER_REPLY {text}
        Render->>Core: processUserReply() [archives only - no re-eval]
    else Clean
        Core->>DB: saveExtraction() + line_items + chat_messages
        DB-->>Core: invoice_id
        Core-->>User: LEDGER_UPDATE {deterministic matrix}
    end

    User->>Render: REQUEST_FILING
    Render->>Core: requestFiling()
    alt Cold Start
        Core->>DB: SELECT document_json FROM invoices
        DB-->>Core: Raw JSON
        Core->>Core: validateAndSanitize() rehydration
    end
    Core->>Core: GSTRGenerator::gstr1B2B / gstr1HsnSummary / gstr3B
    Core-->>User: FILING_READY {gstr1_b2b.csv, gstr1_hsn_summary.csv, gstr3b_summary.csv}
    Core->>DB: UPDATE tax_sessions SET status='FILED'
```

#### 3. Reconciled SQLite Entity-Relationship Diagram (`erDiagram`)

```mermaid
erDiagram
    USERS ||--o{ SESSIONS : "user_id FK"
    USERS {
        INTEGER id PK "AUTOINCREMENT"
        TEXT username
        TEXT email UK "UNIQUE, normalized lowercase"
        TEXT password_hash "PBKDF2-SHA256$100k$16B-salt$32B-hash"
        TEXT role "DEFAULT 'user' / 'admin'"
        INTEGER message_count "DEFAULT 0"
        TEXT created_at "ISO-8601 UTC"
    }
    SESSIONS {
        TEXT token_hash PK "SHA-256 of 32B token"
        INTEGER user_id FK "REFERENCES users(id) ON DELETE CASCADE"
        INTEGER expires_at "UNIX epoch"
        INTEGER created_at "UNIX epoch DEFAULT now"
    }
    TAX_SESSIONS ||--o{ INVOICES : "session_id FK"
    TAX_SESSIONS ||--o{ CHAT_MESSAGES : "session_id FK"
    TAX_SESSIONS {
        TEXT session_id PK "UUID v4"
        TEXT status "CHECK ACTIVE|INTERROGATING|FILED|ABANDONED"
        TEXT rolling_abstract "STUBBED - no compaction logic"
        TEXT created_at "ISO-8601 UTC"
        TEXT updated_at "ISO-8601 UTC"
    }
    INVOICES ||--o{ LINE_ITEMS : "invoice_id FK"
    INVOICES {
        INTEGER invoice_id PK "AUTOINCREMENT"
        TEXT session_id FK "REFERENCES tax_sessions ON DELETE CASCADE"
        TEXT file_name
        TEXT mime_type
        TEXT invoice_number
        TEXT invoice_date "YYYY-MM-DD"
        TEXT seller_gstin
        TEXT buyer_gstin
        TEXT classification "CHECK B2B|B2C|UNDETERMINED"
        TEXT supply_type "CHECK INTRA|INTER"
        TEXT place_of_supply
        INTEGER irn_present "0|1"
        TEXT irn
        TEXT hsn_string
        INTEGER base_value_cents "INT64"
        INTEGER total_tax_cents "INT64"
        INTEGER total_value_cents "INT64"
        TEXT tax_breakdown_json
        TEXT extraction_meta_json
        TEXT document_json "LOSSLESS raw extraction for rehydration"
        TEXT extracted_at "ISO-8601 UTC"
    }
    LINE_ITEMS {
        INTEGER line_item_id PK "AUTOINCREMENT"
        INTEGER invoice_id FK "REFERENCES invoices ON DELETE CASCADE"
        INTEGER line_no
        TEXT hsn "Digits only, ≤8"
        TEXT description
        REAL quantity
        INTEGER unit_price_cents
        INTEGER base_value_cents "INT64"
        INTEGER tax_rate_bps "Statutory slabs only"
        TEXT note
    }
    CHAT_MESSAGES {
        INTEGER message_id PK "AUTOINCREMENT"
        TEXT session_id FK "REFERENCES tax_sessions ON DELETE CASCADE"
        TEXT role "CHECK assistant|user|system"
        TEXT content
        TEXT action_required "INPUT_TEXT or NULL"
        TEXT created_at "ISO-8601 UTC"
    }
```

---

**Append confirmed:**
- Starting line of `---` separator: **741** (after Section 10's closing `---` at line 739)
- New total line count of `./final_sheet.md`: **~1,120 lines** (appended ~390 lines)
- **Zero other files modified** — append-only to `./final_sheet.md` as instructed.
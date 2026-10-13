# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

AI-Powered Smart Tax Compliance Assistant for Indian MSMEs. Automates multimodal invoice ingestion, performs deterministic 2026 GST rule validation, and serializes validated data into GSTR-1/GSTR-3B filing formats.

## Tech Stack

- **Backend:** C++17 with Crow framework (WebSocket server)
- **Frontend:** HTML5/CSS3/Vanilla JS (single-page app)
- **Database:** SQLite3 (embedded)
- **AI Integration:** Google Gemini Pro API (multimodal reasoning)
- **Build System:** CMake 3.15+
- **JSON Library:** nlohmann/json (single header)
- **HTTP Client:** libcurl

## Build Commands

```bash
# Generate build configuration
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Compile the server binary
cmake --build build --config Release

# Server output
./build/tax_assistant_server
```

## Running the Application

```bash
# 1. Load environment variables and start backend
export $(cat .env | xargs)
./build/tax_assistant_server

# 2. Serve frontend (separate terminal)
cd frontend && python3 -m http.server 3000

# Access: http://localhost:3000
# WebSocket: ws://localhost:8080/ws
```

## Architecture

### Core Design: Decoupled Cloud-Hybrid

- **Probabilistic Layer:** Gemini Pro handles multimodal extraction (invoice parsing to JSON)
- **Deterministic Layer:** C++ backend handles all math (CGST/SGST/IGST splits) with 0% hallucination guarantee
- **Rolling Abstract Memory:** Compresses conversation history to prevent token bloat across multi-turn sessions

### Key Components

- `StateMatrix` — In-memory tax state representation; immutable state matrix for calculations
- `RuleEngine` — Configuration-driven compliance verification (rules in `config/rules_2026.json`)
- `GeminiClient` — HTTPS client for Gemini Pro API with strict JSON schema enforcement
- `GSTRGenerator` — CSV serialization for GSTR-1 B2B, HSN Summary, and GSTR-3B
- `WebSocketServer` — Crow-based endpoint router for full-duplex communication

### Data Flow

1. Client uploads invoice (Base64) via WebSocket
2. Backend forwards to Gemini Pro with strict schema
3. Extracted data validated against `config/rules_2026.json`
4. Gaps trigger targeted conversational prompts
5. Complete data serialized to GST Offline Utility CSV format

## WebSocket Protocol

### Client → Server

```json
{ "type": "UPLOAD_INVOICE", "payload": "<BASE64>", "mime_type": "image/jpeg" }
{ "type": "USER_REPLY", "text": "..." }
{ "type": "REQUEST_FILING" }
```

### Server → Client

```json
{ "type": "SESSION_INIT", "session_id": "..." }
{ "type": "CHAT_MESSAGE", "role": "assistant", "content": "...", "action_required": "INPUT_TEXT" }
{ "type": "FILING_READY", "files": { "gstr1_b2b.csv": "...", "gstr3b_summary.csv": "..." } }
```

## Environment Variables

```bash
PORT=8080
HOST=0.0.0.0
GEMINI_API_KEY=<your_key>
GEMINI_MODEL=gemini-3.7-flash
DATABASE_PATH=./database/tax_sessions.db
RULES_CONFIG_PATH=./config/rules_2026.json
```

## GST Compliance Rules (2026)

- E-invoicing mandatory for AATO > ₹5 Crore
- 30-day reporting window on IRP
- Strict IGST vs CGST+SGST validation based on inter/intra-state
- 4-digit and 6-digit HSN code verification

## Security Model

- All financial data stored locally in SQLite (zero cloud storage)
- API keys via environment variables only (never committed)
- Gemini API receives only targeted invoice buffers for extraction

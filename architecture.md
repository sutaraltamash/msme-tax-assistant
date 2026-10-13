# **AI-Powered Smart Tax Compliance Assistant for MSMEs**

An enterprise-grade, cloud-hybrid compliance assistant engineered specifically for Indian Micro, Small, and Medium Enterprises (MSMEs). The platform automates multimodal invoice ingestion, performs deterministic 2026 GST rule validation, eliminates Large Language Model (LLM) context drift via a Rolling Abstract memory architecture, and serializes validated financial data into ready-to-file GSTR-1 and GSTR-3B offline utility formats.

## **1\. System Overview & Core Innovation**

Traditional tax automation pipelines rely on disjointed OCR-to-NER engines that suffer from cascading recognition errors, or generic cloud LLM wrappers that exhibit context drift and mathematical hallucinations.  
This platform solves both failure modes through a **Decoupled Cloud-Hybrid Architecture**:

* **Probabilistic Visual Extraction & Reasoning:** Offloaded to Google's frontier multimodal reasoning model (Gemini Pro) via secure, structured API calls, extracting spatial invoice data directly into strict JSON schemas without an intermediate OCR layer.  
* **Deterministic Calculation & State Tracking:** Handled exclusively by a high-performance C++ backend. All mathematical splits (CGST, SGST, IGST), threshold verifications, and compliance checks are executed in native code using an immutable State Matrix, guaranteeing 0% mathematical hallucination.  
* **Rolling Abstract Memory Engine:** To prevent token bloat and context loss across multi-turn conversations, the C++ backend periodically compresses conversational history into an asynchronous 3-sentence summary, injecting only the immutable ledger and latest state into subsequent reasoning prompts.

## **2\. Key Features**

* **Multimodal Document Parsing:** Drag-and-drop ingestion of raw invoices (PDF, PNG, JPG) converted into Base64 streams and parsed into structured line items.  
* **Real-Time WebSocket Pipeline:** Full-duplex client-server communication supporting token-by-token streaming responses for a snappy user experience.  
* **Configuration-Driven Rule Engine (CDA):** Compliance rules are decoupled from compiled C++ binaries into config/rules\_2026.json. Supports 2026 mandates:  
  * Mandatory electronic invoicing (e-invoicing) verification for Aggregate Annual Turnover (AATO) exceeding ₹5 Crore.  
  * 30-day reporting window validation on the Invoice Registration Portal (IRP).  
  * Strict validation of inter-state (IGST) versus intra-state (CGST \+ SGST) tax allocation.  
  * Mandatory 4-digit and 6-digit HSN code verification.  
* **Dynamic Gap Interrogation:** Automatically detects missing fields (e.g., undetermined B2B vs. B2C status) and generates targeted conversational prompts to resolve ambiguities.  
* **Audit & Loophole Detection:** Identifies tax risks prior to submission (e.g., claiming Input Tax Credit on purchases from Composition Scheme dealers).  
* **Automated GSTR Serialization:** Compiles validated session data into official GST Offline Utility CSV formats (GSTR-1 B2B, GSTR-1 HSN Summary, and GSTR-3B Monthly Return).

## **3\. Architecture & Data Flow**

\[ Client Browser (HTML5/CSS3/Vanilla JS) \]  
                   │  
                   │  (1) Upload PDF/Image (Base64) & Prompts via WebSocket  
                   ▼  
\[ C++ WebSocket Gateway (Crow Framework / C++17) \]  
    │              │  
    │ (2) Ingest   │ (3) Forward Payload with Strict Schema  
    ▼              ▼  
\[ SQLite3 DB \]  \[ Gemini Pro Reasoning API \]  
(tax\_sessions)     │  
    ▲              │ (4) Return Strict JSON Data  
    │              ▼  
    │       \[ C++ State Matrix Core (nlohmann/json) \]  
    │              │  
    │              │ (5) Cross-Reference  
    │              ▼  
    │       \[ Configuration-Driven Rule Engine \] \<── \[ config/rules\_2026.json \]  
    │              │  
    │       ┌──────┴─────────────────────────────────┐  
    │       ▼                                        ▼  
    │  \[ Gaps Detected \]                     \[ Verification Complete \]  
    │  Generate Micro-Prompt                 Serialize Validated Ledger  
    │  to Interrogate User                               │  
    │       │                                            ▼  
    └───────┼────────────────────────────── \[ GSTR Utility Exporter \]  
            ▼                                            │  
   (Stream Chat Response)                                ▼  
                                             (Deliver Ready-to-File CSVs)

## **4\. Repository Structure**

Plaintext  
tax\_assistant\_project/  
├── .env.example              \# Environment variable configuration template  
├── .gitignore                \# Git exclusion rules  
├── CMakeLists.txt            \# CMake compilation & linking definitions  
├── README.md                 \# System documentation  
├── config/  
│   └── rules\_2026.json       \# Declarative 2026 GST statutory compliance rules  
├── database/  
│   └── schema.sql            \# SQLite relational tables for sessions & invoices  
├── frontend/  
│   ├── index.html            \# Single Page Application UI layout  
│   ├── styles.css            \# Stylesheet (Responsive neutral theme)  
│   ├── app.js                \# Core frontend event handling & UI state  
│   ├── websocket\_client.js   \# Full-duplex WebSocket client wrapper  
│   └── file\_handler.js       \# File drag-and-drop & Base64 encoder  
└── backend/  
    ├── include/  
    │   ├── json.hpp          \# nlohmann/json single header library  
    │   ├── sqlite3.h         \# SQLite3 C/C++ interface  
    │   ├── StateMatrix.hpp   \# In-memory tax state representation & storage  
    │   ├── RuleEngine.hpp    \# Dynamic JSON rule verification engine  
    │   ├── GeminiClient.hpp  \# HTTPS client for Gemini Pro reasoning API  
    │   ├── GSTRGenerator.hpp \# CSV serialization for GSTR-1 & GSTR-3B  
    │   └── WebSocketServer.hpp \# Crow WebSocket endpoint router  
    └── src/  
        ├── main.cpp          \# Application entrypoint & initialization  
        ├── StateMatrix.cpp  
        ├── RuleEngine.cpp  
        ├── GeminiClient.cpp  
        ├── GSTRGenerator.cpp  
        └── WebSocketServer.cpp

## **5\. Prerequisites & Dependencies**

### **Operating System**

* Linux (Ubuntu 20.04 LTS / 22.04 LTS recommended), macOS, or Windows Subsystem for Linux (WSL2).

### **System Packages & Libraries**

Ensure the following packages and header files are installed on the host system:

* **C++ Compiler:** g++ (supporting C++17 or C++20) or clang++  
* **Build System:** cmake (version 3.15 or higher)  
* **Networking & TLS:** libcurl4-openssl-dev  
* **Database Engine:** libsqlite3-dev  
* **Concurrency:** libpthread

Install all dependencies on Debian/Ubuntu systems using:

Bash  
sudo apt-get update  
sudo apt-get install \-y build-essential cmake libcurl4-openssl-dev libsqlite3-dev

## **6\. Environment Configuration**

> 1. Clone the repository to your local environment:  
>    Bash  
>    git clone https://github.com/your-username/tax\_assistant\_project.git  
>    cd tax\_assistant\_project

> 2. Create a runtime configuration file from the provided template:  
>    Bash  
>    cp .env.example .env

> 3. Populate .env with your active service credentials:  
>    Code snippet  
>    \# Network Server Settings  
>    PORT=8080  
>    HOST=0.0.0.0

>    \# AI Integration Settings  
>    GEMINI\_API\_KEY=your\_google\_ai\_studio\_api\_key\_here  
>    GEMINI\_MODEL=gemini-3.7-flash  
>    GEMINI\_API\_ENDPOINT=https://generativelanguage.googleapis.com/v1beta/models/gemini-3.7-flash:generateContent

>    \# File Paths  
>    DATABASE\_PATH=./database/tax\_sessions.db  
>    RULES\_CONFIG\_PATH=./config/rules\_2026.json

## **7\. Build & Compilation Instructions**

The backend relies on CMake for dependency resolution and binary compilation.

Bash  
\# Generate build configuration  
cmake \-B build \-DCMAKE\_BUILD\_TYPE=Release

\# Compile the server binary  
cmake \--build build \--config Release

Upon successful compilation, the server executable will be located at:

Plaintext  
./build/tax\_assistant\_server

## **8\. Running the Application**

> 1. **Initialize the Database & Start the Backend:**  
>    Ensure your environment variables are sourced and launch the executable:  
>    Bash  
>    export $(cat .env | xargs)  
>    ./build/tax\_assistant\_server

>    The server will start listening for persistent WebSocket connections on ws://0.0.0.0:8080/ws.  
> 2. **Launch the Web Interface:**  
>    Open frontend/index.html directly in any modern web browser, or serve it using a lightweight local HTTP server:  
>    Bash  
>    cd frontend  
>    python3 \-m http.server 3000

>    Access the client interface at http://localhost:3000.

## **9\. Verification & Execution Lifecycle**

> 1. **Document Ingestion:**  
>    Upload an invoice image or PDF via the drag-and-drop boundary. The file is encoded to Base64 in file\_handler.js and transmitted through the WebSocket gateway.  
> 2. **Extraction:**  
>    GeminiClient.cpp queries the multimodal endpoint with strict JSON schema instructions. Extracted invoice properties (GSTINs, HSN codes, invoice dates, base values, tax totals) are returned to the C++ core.  
> 3. **Validation & State Check:**  
>    RuleEngine.cpp validates the extracted properties against config/rules\_2026.json.  
> 4. **Targeted Interrogation:**  
>    If parameters are incomplete (e.g., missing B2B indicator or missing IRN for an entity with \>₹5 Crore turnover), the server streams a contextual question to the UI.  
> 5. **Filing Export:**  
>    Once all fields are validated, the user can trigger filing generation. GSTRGenerator.cpp maps the in-memory structs to standardized CSV outputs ready for immediate upload to the GST Offline Tool.

## **10\. API Specification & Payloads**

### **WebSocket Protocol (ws://localhost:8080/ws)**

#### **Client to Server Payloads**

* **Invoice Ingestion:**  
  JSON  
  {  
    "type": "UPLOAD\_INVOICE",  
    "payload": "\<BASE64\_ENCODED\_FILE\_STRING\>",  
    "mime\_type": "image/jpeg"  
  }

* **User Interrogation Response:**  
  JSON  
  {  
    "type": "USER\_REPLY",  
    "text": "This invoice is an inter-state B2B transaction."  
  }

* **Trigger Final Export:**  
  JSON  
  {  
    "type": "REQUEST\_FILING"  
  }

#### **Server to Client Payloads**

* **Session Initialization:**  
  JSON  
  {  
    "type": "SESSION\_INIT",  
    "session\_id": "4a7c88b2-132d-4299-b1d5-fa2e8964099b"  
  }

* **Interrogation / Guidance Stream:**  
  JSON  
  {  
    "type": "CHAT\_MESSAGE",  
    "role": "assistant",  
    "content": "The invoice exceeds statutory e-invoicing limits, but no IRN was detected. Please provide the 64-character IRN string or confirm exemption status.",  
    "action\_required": "INPUT\_TEXT"  
  }

* **Filing Deliverable Ready:**  
  JSON  
  {  
    "type": "FILING\_READY",  
    "files": {  
      "gstr1\_b2b.csv": "GSTIN/UIN,Invoice Number,Invoice Date,Invoice Value,Place of Supply,Rate,Taxable Value\\n...",  
      "gstr3b\_summary.csv": "Nature of Supplies,Total Taxable Value,Integrated Tax,Central Tax,State/UT Tax,Cess\\n..."  
    }  
  }

## **11\. Security & Privacy**

* **Zero Cloud Storage of Financial Ledgers:** All user records, parsed structs, and rolling context logs reside exclusively on the host server in an embedded SQLite database (tax\_sessions.db).  
* **Transient API Usage:** Outbound requests to the Gemini API contain only the targeted invoice buffer for immediate feature extraction; no long-term external data retention is utilized.  
* **Credential Isolation:** API keys are injected at runtime via environment variables and are excluded from version control via .gitignore.
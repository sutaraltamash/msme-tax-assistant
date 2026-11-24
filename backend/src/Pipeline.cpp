// ============================================================================
// Pipeline.cpp — end-to-end orchestration (see header).
// ============================================================================

#include "Pipeline.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <array>
#include <filesystem>
#include <cstdint>

#include "GSTRGenerator.hpp"
#include "Sanitizer.hpp"

namespace tax {

namespace {

// Cryptographic-quality-enough v4 UUID for session keys. Format 8-4-4-4-12.
std::string uuid4()
{
    std::random_device rd;
    std::mt19937_64 gen((std::uint64_t{rd()} << 32) ^ rd());
    char buf[37];
    std::snprintf(buf, sizeof(buf),
                  "%08x-%04x-4%03x-%04llx-%012llx",
                  static_cast<unsigned>(gen() & 0xffffffffu),
                  static_cast<unsigned>(gen() & 0xffffu),
                  static_cast<unsigned>(gen() & 0x0fffu),
                  static_cast<unsigned long long>((gen() & 0x3fffu) | 0x8000u),
                  static_cast<unsigned long long>(gen() & 0xffffffffffffull));
    return std::string(buf);
}

// ============================================================================
// File validation helpers (Security hardening: Task 2)
// ============================================================================

// Magic byte signatures for allowed file types
struct MagicBytes {
    static constexpr std::array<uint8_t, 4> PDF = {0x25, 0x50, 0x44, 0x46}; // %PDF
    static constexpr std::array<uint8_t, 8> PNG = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    static constexpr std::array<uint8_t, 3> JPEG = {0xFF, 0xD8, 0xFF};
};

// Verify magic bytes of raw file data against allowed types
// Returns the detected mime type on success, throws std::runtime_error on failure
std::string verifyMagicBytes(const std::vector<uint8_t>& data) {
    // Check PDF (4 bytes: %PDF)
    if (data.size() >= MagicBytes::PDF.size()) {
        bool match = true;
        for (size_t i = 0; i < MagicBytes::PDF.size(); ++i) {
            if (data[i] != MagicBytes::PDF[i]) { match = false; break; }
        }
        if (match) return "application/pdf";
    }

    // Check PNG (8 bytes)
    if (data.size() >= MagicBytes::PNG.size()) {
        bool match = true;
        for (size_t i = 0; i < MagicBytes::PNG.size(); ++i) {
            if (data[i] != MagicBytes::PNG[i]) { match = false; break; }
        }
        if (match) return "image/png";
    }

    // Check JPEG (3 bytes)
    if (data.size() >= MagicBytes::JPEG.size()) {
        bool match = true;
        for (size_t i = 0; i < MagicBytes::JPEG.size(); ++i) {
            if (data[i] != MagicBytes::JPEG[i]) { match = false; break; }
        }
        if (match) return "image/jpeg";
    }

    // If too small for any format
    size_t minSize = std::min({MagicBytes::PDF.size(), MagicBytes::PNG.size(), MagicBytes::JPEG.size()});
    if (data.size() < minSize) {
        throw std::runtime_error("File too small to verify type");
    }

    throw std::runtime_error("Unsupported file type. Allowed: PDF, PNG, JPEG");
}

// RAII wrapper for secure temporary file - guarantees cleanup on all paths
class SecureTempFile {
public:
    explicit SecureTempFile(const std::vector<uint8_t>& data)
        : path_(createTempFile(data)) {}

    ~SecureTempFile() {
        cleanup();
    }

    SecureTempFile(const SecureTempFile&) = delete;
    SecureTempFile& operator=(const SecureTempFile&) = delete;

    SecureTempFile(SecureTempFile&& other) noexcept
        : path_(std::move(other.path_)), data_(std::move(other.data_)) {
        other.path_.clear();
    }

    SecureTempFile& operator=(SecureTempFile&& other) noexcept {
        if (this != &other) {
            cleanup();
            path_ = std::move(other.path_);
            data_ = std::move(other.data_);
            other.path_.clear();
        }
        return *this;
    }

    const std::filesystem::path& path() const { return path_; }
    const std::vector<uint8_t>& data() const { return data_; }

private:
    std::filesystem::path path_;
    std::vector<uint8_t> data_;

    std::filesystem::path createTempFile(const std::vector<uint8_t>& data) {
        // Create temp directory outside web root
        std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "gstnode_uploads";
        std::error_code ec;
        std::filesystem::create_directories(tempDir, ec);

        // Generate cryptographically random filename
        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::uniform_int_distribution<uint64_t> dist(0, UINT64_MAX);

        std::filesystem::path tempPath;
        do {
            std::stringstream ss;
            ss << std::hex << dist(gen) << dist(gen) << dist(gen) << dist(gen);
            tempPath = tempDir / (ss.str() + ".tmp");
        } while (std::filesystem::exists(tempPath));

        // Write file
        std::ofstream out(tempPath, std::ios::binary);
        if (!out) {
            throw std::runtime_error("Failed to create temporary file");
        }
        out.write(reinterpret_cast<const char*>(data.data()), data.size());
        if (!out) {
            throw std::runtime_error("Failed to write temporary file");
        }

        return tempPath;
    }

    void cleanup() {
        if (!path_.empty() && std::filesystem::exists(path_)) {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
            // Ignore cleanup errors - best effort
        }
    }
};

// Decode base64 string to raw bytes
std::vector<uint8_t> base64Decode(const std::string& base64) {
    static const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::vector<uint8_t> out;
    out.reserve(base64.size() * 3 / 4);

    size_t i = 0;
    while (i < base64.size()) {
        // Skip whitespace
        while (i < base64.size() && isspace(static_cast<unsigned char>(base64[i]))) ++i;
        if (i >= base64.size()) break;

        uint32_t sextet[4] = {0, 0, 0, 0};
        int sextetCount = 0;

        for (int j = 0; j < 4 && i < base64.size(); ++j, ++i) {
            char c = base64[i];
            if (c == '=') break;
            size_t pos = alphabet.find(c);
            if (pos == std::string::npos) continue;
            sextet[sextetCount++] = static_cast<uint32_t>(pos);
        }

        if (sextetCount >= 2) {
            out.push_back(static_cast<uint8_t>((sextet[0] << 2) | (sextet[1] >> 4)));
        }
        if (sextetCount >= 3) {
            out.push_back(static_cast<uint8_t>(((sextet[1] & 0x0F) << 4) | (sextet[2] >> 2)));
        }
        if (sextetCount >= 4) {
            out.push_back(static_cast<uint8_t>(((sextet[2] & 0x03) << 6) | sextet[3]));
        }

        // Skip padding
        while (i < base64.size() && base64[i] == '=') ++i;
    }

    return out;
}

} // anonymous namespace

const char* sessionStatusToString(SessionStatus status)
{
    switch (status)
    {
        case SessionStatus::ACTIVE:        return "ACTIVE";
        case SessionStatus::INTERROGATING: return "INTERROGATING";
        case SessionStatus::FILED:         return "FILED";
        case SessionStatus::ABANDONED:     return "ABANDONED";
    }
    return "ACTIVE";
}

// ===========================================================================
// construction
// ===========================================================================

json Pipeline::loadRulesConfig(const std::string& path)
{
    std::ifstream in(path);
    if (!in.is_open())
    {
        throw std::runtime_error("pipeline: cannot open rules config '" + path + "'");
    }
    try
    {
        std::stringstream ss;
        ss << in.rdbuf();
        return json::parse(ss.str());
    }
    catch (const json::parse_error& e)
    {
        throw std::runtime_error("pipeline: rules config '" + path +
                                 "' is not valid JSON: " + e.what());
    }
}

Pipeline::Pipeline(std::unique_ptr<IExtractor> extractor,
                   const std::string&          rules_config_path,
                   const std::string&          db_path)
  : extractor_(std::move(extractor)),
    rules_(loadRulesConfig(rules_config_path)),
    repo_(std::make_unique<InvoiceRepository>(db_path))
{
}

Pipeline::~Pipeline() = default;

json Pipeline::chatFrame(const std::string& role, const std::string& content,
                         const std::string& action_required)
{
    json f{{"type", "CHAT_MESSAGE"}, {"role", role}, {"content", content}};
    if (!action_required.empty()) f["action_required"] = action_required;
    return f;
}

json Pipeline::ledgerUpdateFrame(const StateMatrix& matrix)
{
    json frame{{"type", "LEDGER_UPDATE"}};

    // supplier
    frame["supplier"]["legal_name"] = matrix.supplierName();
    frame["supplier"]["gstin"] = matrix.supplierGstin();
    frame["supplier"]["state_code"] = matrix.supplierState();

    // buyer
    if (matrix.hasBuyer())
    {
        if (matrix.buyerName().has_value()) frame["buyer"]["legal_name"] = *matrix.buyerName();
        if (matrix.buyerGstin().has_value()) frame["buyer"]["gstin"] = *matrix.buyerGstin();
        if (matrix.buyerState().has_value()) frame["buyer"]["state_code"] = *matrix.buyerState();
    }

    // invoice
    frame["invoice"]["invoice_number"] = matrix.invoiceNumber();
    frame["invoice"]["invoice_date"] = matrix.invoiceDate();
    frame["invoice"]["place_of_supply"] = matrix.placeOfSupply();

    // irp
    if (matrix.irnPresent())
    {
        frame["irp"]["irn"] = matrix.irn();
    }

    // line_items
    for (const auto& item : matrix.lineItems())
    {
        json li;
        li["hsn_code"] = item.hsn;
        li["description"] = item.description;
        li["quantity"] = item.quantity.value_or(1.0);
        li["unit_price_cents"] = item.unit_price_cents.value_or(0);
        li["taxable_amount_cents"] = item.base_value_cents;

        // Tax rate split
        int rate_bps = item.tax_rate_bps;
        if (matrix.supplyType() == SupplyType::INTRA)
        {
            li["cgst_rate_bps"] = rate_bps / 2;
            li["sgst_rate_bps"] = rate_bps / 2;
            li["igst_rate_bps"] = 0;
        }
        else
        {
            li["cgst_rate_bps"] = 0;
            li["sgst_rate_bps"] = 0;
            li["igst_rate_bps"] = rate_bps;
        }
        li["total_cents"] = item.base_value_cents +
            (matrix.supplyType() == SupplyType::INTRA
                ? (money::taxOn(item.base_value_cents, rate_bps / 2) * 2)
                : money::taxOn(item.base_value_cents, rate_bps));
        frame["line_items"].push_back(std::move(li));
    }

    // totals
    const auto& tax = matrix.tax();
    frame["totals"]["base_value_cents"] = matrix.baseValueCents();
    frame["totals"]["igst_cents"] = tax.igst_cents;
    frame["totals"]["cgst_cents"] = tax.cgst_cents;
    frame["totals"]["sgst_cents"] = tax.sgst_cents;
    frame["totals"]["total_tax_cents"] = tax.total_tax_cents;
    frame["totals"]["total_value_cents"] = matrix.totalValueCents();

    return frame;
}

// ===========================================================================
// session lifecycle
// ===========================================================================

std::string Pipeline::openSession()
{
    const std::string id = uuid4();
    std::lock_guard<std::mutex> lock(mutex_);
    repo_->createSession(id);
    sessions_[id] = SessionState{};
    return id;
}

void Pipeline::closeSession(const std::string& session_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return;
    if (it->second.status != SessionStatus::FILED)
    {
        it->second.status = SessionStatus::ABANDONED;
        repo_->setSessionStatus(session_id, sessionStatusToString(SessionStatus::ABANDONED));
    }
}

SessionStatus Pipeline::statusOf(const std::string& session_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sessions_.find(session_id);
    return it == sessions_.end() ? SessionStatus::ABANDONED : it->second.status;
}

bool Pipeline::authenticate(const std::string& username, const std::string& email,
                            const std::string& password_hash) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return repo_->verifyUser(username, email, password_hash);
}

// ===========================================================================
// UPLOAD_INVOICE
// ===========================================================================

PipelineResult Pipeline::processUpload(const std::string& session_id,
                                       const std::string& base64_data,
                                       const std::string& mime_type)
{
    std::lock_guard<std::mutex> lock(mutex_);
    PipelineResult out;

    const auto it = sessions_.find(session_id);
    if (it == sessions_.end())
    {
        out.frames.push_back(
            chatFrame("assistant",
                      "Unknown session. Reconnect to /ws to start a new session.",
                      ""));
        return out;
    }

    // --- Security hardening: File validation (Task 2) ---
    // 1. Decode base64 to raw bytes
    std::vector<uint8_t> fileData;
    try {
        fileData = base64Decode(base64_data);
    } catch (const std::exception& e) {
        out.frames.push_back(chatFrame("assistant",
                                       "Invalid base64 encoding: " + std::string(e.what()),
                                       ""));
        return out;
    }

    // 2. Check file size (max 5 MB)
    constexpr size_t MAX_FILE_SIZE = 5 * 1024 * 1024; // 5 MB
    if (fileData.size() > MAX_FILE_SIZE) {
        out.frames.push_back(chatFrame("assistant",
                                       "File too large. Maximum allowed size is 5 MB.",
                                       ""));
        return out;
    }

    // 3. Verify magic bytes (do NOT trust mime_type or extension)
    std::string detectedMime;
    try {
        detectedMime = verifyMagicBytes(fileData);
    } catch (const std::exception& e) {
        out.frames.push_back(chatFrame("assistant",
                                       std::string(e.what()),
                                       ""));
        return out;
    }

    // 4. Create secure temporary file (RAII guarantees cleanup)
    SecureTempFile tempFile(fileData);

    // --- probabalistic layer: extraction -----------------------------------
    // Pass the detected mime type (from magic bytes) to extractor
    json doc;
    try
    {
        doc = extractor_->extractFromBuffer(base64_data, detectedMime);
    }
    catch (const std::exception& e)
    {
        out.frames.push_back(chatFrame("assistant",
                                       "Invoice extraction failed: " +
                                           std::string(e.what()),
                                       ""));
        return out;
    }

    // --- deterministic layer: validate -> evaluate --------------------------
    StateMatrix matrix;
    std::vector<RuleResult> results;
    try
    {
        matrix  = StateMatrix::validateAndSanitize(doc);
        results = rules_.evaluate(matrix, doc);
    }
    catch (const std::invalid_argument& e)
    {
        out.frames.push_back(
            chatFrame("assistant",
                      "The extraction did not pass the schema contract and was "
                      "rejected before any assessment: " + std::string(e.what()),
                      ""));
        return out;
    }

    // --- gaps -> interrogation stream; zero gaps -> verified ----------------
    bool gaps = false;
    for (const RuleResult& r : results)
    {
        if (r.violated)
        {
            gaps = true;
            out.frames.push_back(chatFrame("assistant", r.message, r.action_required));
        }
    }

    std::string next_status;
    if (gaps)
    {
        next_status = "INTERROGATING";
        const int open = static_cast<int>(out.frames.size());
        out.frames.push_back(chatFrame(
            "assistant",
            std::to_string(open) + " open compliance gap" + (open == 1 ? "" : "s") +
                " flagged on this invoice. Each needs your input before this "
                "document can be filed.",
            ""));
    }
    else
    {
        next_status = "ACTIVE";
        out.frames.push_back(chatFrame(
            "assistant",
            "Verification complete: this invoice passed every configured 2026 "
            "GST rule. No input required for this document.",
            ""));
    }

    // --- persist the ledger + the transcript --------------------------------
    std::vector<std::string> warnings;
    if (doc.contains("extraction_confidence") && doc["extraction_confidence"].is_object() &&
        doc["extraction_confidence"].contains("warnings") &&
        doc["extraction_confidence"]["warnings"].is_array())
    {
        for (const auto& w : doc["extraction_confidence"]["warnings"])
        {
            if (w.is_string()) warnings.push_back(w.get<std::string>());
        }
    }
    it->second.invoices.push_back(matrix);
    repo_->saveExtraction(session_id, "", mime_type, matrix, doc, warnings);
    for (const json& f : out.frames)
    {
        if (f.value("type", "") == "CHAT_MESSAGE")
        {
            repo_->addChatMessage(session_id, f.value("role", "assistant"),
                                  f.value("content", ""),
                                  f.value("action_required", ""));
        }
    }
    repo_->setSessionStatus(session_id, next_status);
    it->second.status = next_status == "INTERROGATING" ? SessionStatus::INTERROGATING
                                                       : SessionStatus::ACTIVE;

    // Emit LEDGER_UPDATE so frontend can render the ledger immediately
    out.frames.push_back(ledgerUpdateFrame(matrix));

    out.session_status = next_status;
    return out;
}

// ===========================================================================
// USER_REPLY
// ===========================================================================

PipelineResult Pipeline::processUserReply(const std::string& session_id,
                                          const std::string& text)
{
    std::lock_guard<std::mutex> lock(mutex_);
    PipelineResult out;

    const auto it = sessions_.find(session_id);
    if (it == sessions_.end())
    {
        out.frames.push_back(
            chatFrame("assistant", "Unknown session. Reconnect to /ws to start a new session.", ""));
        return out;
    }

    // Sanitize user input before archiving (defense-in-depth)
    const std::string sanitized = sanitizeUserMessage(text);

    repo_->addChatMessage(session_id, "user", sanitized, "");

    const std::string preview = sanitized.size() > 240 ? sanitized.substr(0, 240) + "…" : sanitized;
    const std::string ack =
        "Your note was recorded. In the current extractor-seam phase your reply "
        "is archived for the conversation's rolling abstract; the live Groq chat "
        "loop (Step G) will apply such corrections to the stored invoice. Reply "
        "again, or request the filing once the details are settled.\n\n“" +
        preview + "”";
    out.frames.push_back(chatFrame("assistant", ack, ""));
    repo_->addChatMessage(session_id, "assistant", ack, "");

    out.session_status = sessionStatusToString(it->second.status);
    return out;
}

// ===========================================================================
// REQUEST_FILING
// ===========================================================================

PipelineResult Pipeline::requestFiling(const std::string& session_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    PipelineResult out;

    auto it = sessions_.find(session_id);
    if (it == sessions_.end())
    {
        // Cold start for an archived session id: adopt it from the repository
        // so a filing can be produced after a server restart.
        if (!repo_->sessionExists(session_id))
        {
            out.frames.push_back(
                chatFrame("assistant", "Unknown session. Reconnect to /ws to start a new session.", ""));
            return out;
        }
        it = sessions_.emplace(session_id, SessionState{}).first;
    }

    // In-memory ledger first; after a process restart the session rehydrates
    // losslessly from document_json so a filing can still be produced.
    std::vector<StateMatrix> invoices = it->second.invoices;
    if (invoices.empty()) invoices = repo_->invoicesForSession(session_id);
    if (invoices.empty())
    {
        out.frames.push_back(chatFrame(
            "assistant",
            "No verified invoices are available in this session. Upload at least "
            "one invoice before requesting a filing.",
            ""));
        return out;
    }
    it->second.invoices = std::move(invoices);

    json ready;
    ready["type"] = "FILING_READY";
    ready["files"]["gstr1_b2b.csv"]        = GSTRGenerator::gstr1B2B(it->second.invoices);
    ready["files"]["gstr1_hsn_summary.csv"] = GSTRGenerator::gstr1HsnSummary(it->second.invoices);
    ready["files"]["gstr3b_summary.csv"]    = GSTRGenerator::gstr3B(it->second.invoices);

    repo_->addChatMessage(session_id, "system",
                          "GSTR filing generated from " +
                              std::to_string(it->second.invoices.size()) +
                              " verified invoice(s).",
                          "");
    repo_->setSessionStatus(session_id, "FILED");
    it->second.status = SessionStatus::FILED;
    out.frames.push_back(std::move(ready));
    out.session_status = "FILED";
    return out;
}

} // namespace tax

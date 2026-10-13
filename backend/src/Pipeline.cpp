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

#include "GSTRGenerator.hpp"

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

    // --- probabalistic layer: extraction -----------------------------------
    json doc;
    try
    {
        doc = extractor_->extractFromBuffer(base64_data, mime_type);
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

    repo_->addChatMessage(session_id, "user", text, "");

    const std::string preview = text.size() > 240 ? text.substr(0, 240) + "…" : text;
    const std::string ack =
        "Your note was recorded. In the current extractor-seam phase your reply "
        "is archived for the conversation's rolling abstract; the live Gemini chat "
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
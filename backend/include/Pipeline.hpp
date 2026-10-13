// ============================================================================
// Pipeline — the end-to-end processing orchestration seam (Step F).
//
// Owns the extraction seam (IExtractor), the rule engine, and SQLite persistence,
// and turns the WebSocket message contract (architecture.md §10) into a
// deterministic processing flow:
//
//   UPLOAD_INVOICE -> IExtractor::extractFromBuffer
//                  -> StateMatrix::validateAndSanitize   (fail-fast schema gate)
//                  -> RuleEngine::evaluate               (config/rules_2026.json)
//                  -> InvoiceRepository::saveExtraction  (integer-cents SQLite)
//                  -> stream one CHAT_MESSAGE per open gap (or "verification
//                     complete" when zero gaps), persist the transcript
//
//   USER_REPLY     -> archive the user's text into chat_messages (rolling
//                     abstract input); acknowledge. Resolution semantics land
//                     with the live Gemini chat loop in Step G.
//
//   REQUEST_FILING -> gather the session's matrices (in-memory, falling back to
//                     lossless DB rehydration after a restart) and serialize
//                     GSTR-1 Table 4A + HSN summary + GSTR-3B into a
//                     FILING_READY frame; flip the session to FILED.
//
// Every fraction of the pipeline that performs finance math or validation runs
// in the deterministic integer-cent core; the extractor is the ONLY component
// allowed to be probabilistic, and it is swappable at construction.
//
// Thread safety: every public method is fully serialized behind one mutex
// (Crow may dispatch WebSocket events from several threads).
//
// Output frames are nlohmann::json payloads with a "type" field matching the
// protocol; main.cpp translates them verbatim onto the socket.
// ============================================================================

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "IExtractor.hpp"
#include "InvoiceRepository.hpp"
#include "RuleEngine.hpp"
#include "StateMatrix.hpp"

namespace tax {

enum class SessionStatus : std::uint8_t
{
    ACTIVE,
    INTERROGATING,
    FILED,
    ABANDONED,
};

const char* sessionStatusToString(SessionStatus status);

// The client-visible result of one pipeline step: protocol frames to stream,
// plus the session orientation after the step.
struct PipelineResult
{
    std::vector<json> frames;      // each is a protocol payload (has "type")
    std::string session_status;    // active/… string for the caller
};

class Pipeline
{
public:
    // extractor is moved in; rules_config_path and db_path are read at
    // construction (missing rule file / unopenable DB => std::runtime_error).
    Pipeline(std::unique_ptr<IExtractor> extractor,
             const std::string&          rules_config_path,
             const std::string&          db_path);
    ~Pipeline();

    Pipeline(const Pipeline&)            = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    // ---- session lifecycle ----------------------------------------------------
    // Creates a tax_sessions row and returns its fresh id.
    std::string openSession();
    // Flags a session ABANDONED (no-op when already FILED). Used by WebSocket close.
    void closeSession(const std::string& session_id);

    // ---- protocol handlers ----------------------------------------------------
    PipelineResult processUpload(const std::string& session_id,
                                 const std::string& base64_data,
                                 const std::string& mime_type);
    PipelineResult processUserReply(const std::string& session_id,
                                    const std::string& text);
    PipelineResult requestFiling(const std::string& session_id);

    // Direct access for tests/admin tooling (safe only through the same thread).
    InvoiceRepository& repository() const { return *repo_; }
    SessionStatus      statusOf(const std::string& session_id) const;

private:
    struct SessionState
    {
        SessionStatus status = SessionStatus::ACTIVE;
        std::vector<StateMatrix> invoices;   // in-memory ledger for this process
    };

    // Load + parse a rules file; throws std::runtime_error on any failure.
    static json loadRulesConfig(const std::string& path);

    // Build a {type:"CHAT_MESSAGE",…} frame (action_required omitted when "").
    static json chatFrame(const std::string& role, const std::string& content,
                          const std::string& action_required);

    std::unique_ptr<IExtractor>                      extractor_;
    RuleEngine                                       rules_;
    std::unique_ptr<InvoiceRepository>               repo_;
    std::unordered_map<std::string, SessionState>    sessions_;
    mutable std::mutex                               mutex_;
};

} // namespace tax
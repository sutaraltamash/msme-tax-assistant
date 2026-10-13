// ============================================================================
// IExtractor — inbound document extraction seam.
//
// The decoupled-cloud-hybrid architecture (architecture.md §1) forbids the
// deterministic layer from talking to any cloud or network service directly.
// Extracting a taxable structure from an uploaded invoice buffer is therefore
// the one interaction that borrows a probabilistic model (Gemini Pro). That
// interaction is isolated behind this interface so that:
//
//   * Step F ships a deterministic MockExtractor (no network) that returns a
//     controlled, schema-conforming document — the seam is testable end-to-end
//     before any live API key exists.
//   * Step G's GeminiClient implements THIS interface and is injected at
//     startup, swapping the mock for live extraction without touching the
//     pipeline, persistence, or WebSocket layers.
//
// Implementations MUST return a nlohmann::json document conforming to
// config/gemini_schema.json (validated natively by StateMatrix) and MUST throw
// std::runtime_error on extraction failure rather than returning partial data.
// ============================================================================

#pragma once

#include <string>

#include "json.hpp"

namespace tax {

using nlohmann::json;

class IExtractor
{
public:
    virtual ~IExtractor() = default;

    // Extract a schema-conforming invoice document from a raw file buffer.
    //   base64_data — the uploaded file, Base64-encoded by the client.
    //   mime_type   — "image/jpeg", "application/pdf", etc. (informational).
    // Returns the extracted document. Throws std::runtime_error on failure.
    virtual json extractFromBuffer(const std::string& base64_data,
                                   const std::string& mime_type) const = 0;
};

} // namespace tax
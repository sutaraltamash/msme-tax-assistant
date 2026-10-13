// ============================================================================
// MockExtractor — deterministic stand-in for the IExtractor seam (Step F).
//
// The live GeminiClient (Step G) implements the same IExtractor interface and
// is injected at startup in its place. Until then this mock returns a controlled
// document so the full pipeline (upload -> validate -> rules -> persist -> file)
// is exercised with zero network and zero nondeterminism.
//
// Default document: a fully COMPLIANT inter-state B2B enterprise invoice
// (IRN + IRP metadata present, AATO above the e-invoicing threshold, valid
// 8-digit HSN, IGST-only split) — everything a "0 gaps" extraction looks like,
// so an out-of-the-box upload streams "verification complete". Tests inject a
// document of their choice (e.g. an intra-state B2B invoice missing its IRP
// timestamp) to drive the interrogation branch.
//
// The mock deliberately IGNORES base64_data / mime_type: the seam is proven here,
// the buffer is consumed by the real extractor in Step G.
// ============================================================================

#pragma once

#include <string>

#include "IExtractor.hpp"

namespace tax {

class MockExtractor : public IExtractor
{
public:
    // Use the built-in fully compliant inter-state B2B document.
    MockExtractor();

    // Inject any schema-conforming document; returned verbatim on every call.
    explicit MockExtractor(json document);

    json extractFromBuffer(const std::string& base64_data,
                           const std::string& mime_type) const override;

private:
    // Canonic compliant scaffold (mirrors the inter-state B2B test fixture).
    static json buildDefaultDocument();

    json document_;
};

} // namespace tax
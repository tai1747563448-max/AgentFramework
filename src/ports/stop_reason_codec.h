#pragma once

#include "domain/model_types.h"

#include <optional>
#include <string>
#include <string_view>

namespace agent {

// T12 (v2 §3): adapter-side codec for provider-specific stop reasons.
// The application layer only sees the canonical StopReason enum; the
// AnthropicMessagesClient and any future OpenAI / local adapter map
// their provider-specific token through these helpers so the rest of
// the runtime stays provider-neutral.
//
// Returned StopReason::Unknown is the fail-safe default for
// unrecognised tokens - the runtime treats Unknown as a protocol
// failure downstream so a typo in the decoder never silently slips
// through.

// Map a provider's stop_reason string to the canonical enum. Empty
// input returns StopReason::Unknown rather than std::nullopt so
// adapters can chain the result without unwrapping.
StopReason decode_stop_reason(std::string_view raw);

// Inverse mapping for diagnostics and for adapters that need to
// reserialise a canonical reason back to wire form. Returns the
// canonical Anthropic token for StopReason::Unknown so the result is
// always a printable string.
std::string encode_stop_reason(StopReason reason);

// True when the canonical reason is one the runtime knows how to
// continue from. StopReason::Unknown returns false - tests and the
// runtime both rely on Unknown being a hard failure signal.
bool is_known_stop_reason(StopReason reason);

}  // namespace agent

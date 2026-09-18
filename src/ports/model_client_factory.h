#pragma once

#include "domain/result.h"
#include "ports/model_client.h"

#include <memory>
#include <string>

namespace agent {

class AnthropicConfig;
class HttpTransport;

// T12 (v2 §3): factory that selects a ModelClient adapter from the
// AGENT_PROVIDER environment variable (or a programmatic provider
// name). The only adapter T12 ships is the Anthropic one; the
// factory is the seam OpenAI / local adapters will plug into without
// changing RuntimeEngine or main.cpp.
//
// provider names:
//   "anthropic" (default when AGENT_PROVIDER is unset / unknown)
//
// The factory borrows the HttpTransport by reference - the caller
// owns the transport and keeps it alive for the lifetime of the
// returned ModelClient. The AnthropicConfig is copied because the
// ModelClient is expected to outlive any single config instance.
Result<std::unique_ptr<ModelClient>> create_model_client(
    const std::string& provider_name,
    const AnthropicConfig& anthropic_config,
    HttpTransport& transport);

// Convenience overload: reads AGENT_PROVIDER from the supplied
// environment-style accessor (string -> optional<string>) and
// dispatches to create_model_client. main.cpp wires this in so the
// runtime is provider-pluggable without touching the binary's
// composition root.
Result<std::unique_ptr<ModelClient>> create_model_client_from_provider(
    const std::string& anthropic_provider_value,
    const AnthropicConfig& anthropic_config,
    HttpTransport& transport);

}  // namespace agent

#include "ports/model_client_factory.h"

#include "adapters/anthropic/anthropic_messages_client.h"
#include "adapters/anthropic/http_transport.h"

#include <utility>

namespace agent {

Result<std::unique_ptr<ModelClient>> create_model_client(
    const std::string& provider_name,
    const AnthropicConfig& anthropic_config,
    HttpTransport& transport) {
    // T12 (v2 §3): the only provider adapter shipped is Anthropic.
    // Unknown provider names fall back to Anthropic so an unset /
    // misspelled AGENT_PROVIDER keeps the historical behaviour. The
    // factory is the seam future OpenAI / local adapters plug into
    // without touching RuntimeEngine or main.cpp.
    if (provider_name.empty() || provider_name == "anthropic") {
        return Result<std::unique_ptr<ModelClient>>::success(
            std::make_unique<AnthropicMessagesClient>(anthropic_config,
                                                     transport));
    }
    return Result<std::unique_ptr<ModelClient>>::failure(
        {ErrorCode::InvalidConfiguration,
         "unknown model provider: " + provider_name, false});
}

Result<std::unique_ptr<ModelClient>> create_model_client_from_provider(
    const std::string& anthropic_provider_value,
    const AnthropicConfig& anthropic_config,
    HttpTransport& transport) {
    return create_model_client(anthropic_provider_value, anthropic_config,
                              transport);
}

}  // namespace agent

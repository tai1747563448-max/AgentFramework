#pragma once

#include "adapters/anthropic/http_transport.h"
#include "ports/model_client.h"

#include <cstdint>
#include <string>

namespace agent {

enum class CredentialKind { ApiKey, Bearer };

struct AnthropicConfig {
    std::string base_url;
    std::string model;
    CredentialKind credential_kind{CredentialKind::ApiKey};
    std::string credential;
    std::string api_version;
    std::int64_t max_tokens{0};
};

class AnthropicMessagesClient final : public ModelClient {
public:
    AnthropicMessagesClient(AnthropicConfig config, HttpTransport& transport);

    Result<ModelResponse> complete(const ModelRequest& request) override;
    Result<ModelResponse> complete(const ModelRequest& request,
                                   const ModelCallOptions& options) override;

private:
    AnthropicConfig config_;
    HttpTransport& transport_;
};

}  // namespace agent

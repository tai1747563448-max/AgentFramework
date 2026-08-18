#include "adapters/anthropic/anthropic_messages_client.h"
#include "adapters/anthropic/cpr_http_transport.h"
#include "config/runtime_config.h"
#include "domain/model_types.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <variant>

namespace {

bool safe_request_id(const std::string& request_id) {
    return !request_id.empty() && request_id.size() <= 128 &&
           std::all_of(request_id.begin(), request_id.end(), [](char value) {
               return (value >= 'a' && value <= 'z') ||
                      (value >= 'A' && value <= 'Z') ||
                      (value >= '0' && value <= '9') || value == '-' ||
                      value == '_' || value == '.' || value == ':';
           });
}

bool contains_nonempty_text(const agent::ModelResponse& response) {
    for (const auto& block : response.content) {
        if (const auto* text = std::get_if<agent::TextBlock>(&block)) {
            if (!text->text.empty()) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

int main() {
    agent::ProcessEnvironment environment;
    const auto config = agent::load_runtime_config(environment);
    if (!config.has_value()) {
        std::cout << "status=configuration_error request_id=\n";
        return 2;
    }

    agent::CprHttpTransport transport;
    agent::AnthropicMessagesClient model(config.value().anthropic, transport);
    agent::ModelRequest request;
    request.system_prompt =
        "Return one short, nonempty confirmation sentence. Do not use tools.";
    request.messages = {
        {agent::Role::User,
         {agent::TextBlock{"Confirm that the live smoke request succeeded."}}}};
    request.timeout_ms = config.value().budgets.model_timeout_ms;

    const auto response = model.complete(request);
    if (!response.has_value()) {
        std::cout << "status=request_failed request_id=\n";
        return 1;
    }
    if (!contains_nonempty_text(response.value()) ||
        !safe_request_id(response.value().provider_request_id)) {
        std::cout << "status=invalid_response request_id=\n";
        return 1;
    }

    std::cout << "status=ok request_id="
              << response.value().provider_request_id << '\n';
    return 0;
}

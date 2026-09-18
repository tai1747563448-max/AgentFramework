#include "adapters/tools/composite_tool_gateway.h"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <utility>

namespace agent {

CompositeToolGateway::CompositeToolGateway(
    std::vector<std::reference_wrapper<ToolGateway>> gateways) {
    for (auto& gateway_reference : gateways) {
        auto& gateway = gateway_reference.get();
        for (auto& definition : gateway.definitions()) {
            if (definition.name.empty() ||
                !routes_.emplace(definition.name, &gateway).second) {
                throw std::invalid_argument(
                    "tool definitions must have unique nonempty names");
            }
            definitions_.push_back(std::move(definition));
        }
    }
}

void CompositeToolGateway::register_runtime(
    std::shared_ptr<ToolGateway> runtime_gateway) {
    if (!runtime_gateway) {
        return;
    }
    auto& gateway = *runtime_gateway;
    for (auto& definition : gateway.definitions()) {
        if (definition.name.empty() ||
            !routes_.emplace(definition.name, &gateway).second) {
            throw std::invalid_argument(
                "tool definitions must have unique nonempty names");
        }
        definitions_.push_back(std::move(definition));
    }
    owned_gateways_.push_back(std::move(runtime_gateway));
}

std::vector<ToolDefinition> CompositeToolGateway::definitions() const {
    return definitions_;
}

Result<ToolResult> CompositeToolGateway::execute(
    const ToolCall& call,
    const ToolExecutionContext& context) {
    const auto route = routes_.find(call.name);
    if (route == routes_.end()) {
        const nlohmann::json content{
            {"error", {{"code", "invalid_arguments"},
                       {"message", "unknown tool"},
                       {"retryable", false}}}};
        return Result<ToolResult>::success(
            {call.id, content.dump(), true});
    }
    return route->second->execute(call, context);
}

bool CompositeToolGateway::tool_is_concurrency_safe(
    const std::string& name) const {
    const auto route = routes_.find(name);
    if (route == routes_.end()) {
        return false;
    }
    return route->second->tool_is_concurrency_safe(name);
}

bool CompositeToolGateway::tool_is_read_only(
    const std::string& name) const {
    const auto route = routes_.find(name);
    if (route == routes_.end()) {
        return false;
    }
    return route->second->tool_is_read_only(name);
}

PermissionDecision CompositeToolGateway::tool_permission_decision(
    const ToolCall& call,
    const ToolExecutionContext& context) const {
    const auto route = routes_.find(call.name);
    if (route == routes_.end()) {
        return PermissionDecision::Ask;
    }
    return route->second->tool_permission_decision(call, context);
}

}  // namespace agent

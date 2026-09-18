#pragma once

#include "ports/tool_gateway.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace agent {

class CompositeToolGateway final : public ToolGateway {
public:
    explicit CompositeToolGateway(
        std::vector<std::reference_wrapper<ToolGateway>> gateways);

    // T14 (v2 §3): register_runtime adopts ownership of an extra
    // gateway (typically an McpToolGateway) and routes every new
    // tool definition through the existing dispatch table. The
    // composite stays the single dispatch surface the runtime sees.
    void register_runtime(std::shared_ptr<ToolGateway> runtime_gateway);

    std::vector<ToolDefinition> definitions() const override;
    Result<ToolResult> execute(
        const ToolCall& call,
        const ToolExecutionContext& context) override;

    bool tool_is_concurrency_safe(
        const std::string& name) const override;
    bool tool_is_read_only(
        const std::string& name) const override;
    PermissionDecision tool_permission_decision(
        const ToolCall& call,
        const ToolExecutionContext& context) const override;

private:
    std::vector<ToolDefinition> definitions_;
    std::unordered_map<std::string, ToolGateway*> routes_;
    std::vector<std::shared_ptr<ToolGateway>> owned_gateways_;
};

}  // namespace agent

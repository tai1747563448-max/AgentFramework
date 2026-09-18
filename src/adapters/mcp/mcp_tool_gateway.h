#pragma once

#include "adapters/mcp/server_registry.h"
#include "ports/tool_gateway.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace agent::mcp {

// T14 (v2 §3): McpToolGateway implements ToolGateway on top of the
// registry. The gateway lazily creates one Tool object per MCP tool
// surfaced by the registry and routes dispatch by "server__tool"
// name. Failed calls synthesise a ToolResult with is_error=true so
// the runtime's existing error-handling path applies without
// change.
//
// The gateway assumes bootstrap_all() has already been called by
// the composition root - this keeps constructor side-effect free
// and lets main.cpp surface a clear error if MCP servers are
// misconfigured.
class McpToolGateway final : public ToolGateway {
public:
    explicit McpToolGateway(ServerRegistry& registry);
    ~McpToolGateway() override;

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
    ServerRegistry& registry_;
    std::unordered_map<std::string, McpTool> tools_;
    std::unordered_map<std::string, std::string> server_for_tool_;
};

}  // namespace agent::mcp

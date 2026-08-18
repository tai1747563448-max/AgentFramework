#include "adapters/empty/empty_tool_gateway.h"

namespace agent {

std::vector<ToolDefinition> EmptyToolGateway::definitions() const {
    return {};
}

Result<ToolResult> EmptyToolGateway::execute(const ToolCall&) {
    return Result<ToolResult>::failure(
        {ErrorCode::DependencyUnavailable, "no tools are configured", false});
}

}  // namespace agent

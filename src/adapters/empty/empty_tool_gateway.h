#pragma once

#include "ports/tool_gateway.h"

namespace agent {

class EmptyToolGateway final : public ToolGateway {
public:
    std::vector<ToolDefinition> definitions() const override;
    Result<ToolResult> execute(
        const ToolCall& call,
        const ToolExecutionContext& context) override;
};

}  // namespace agent

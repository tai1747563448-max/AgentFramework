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

    std::vector<ToolDefinition> definitions() const override;
    Result<ToolResult> execute(
        const ToolCall& call,
        const ToolExecutionContext& context) override;

private:
    std::vector<ToolDefinition> definitions_;
    std::unordered_map<std::string, ToolGateway*> routes_;
};

}  // namespace agent

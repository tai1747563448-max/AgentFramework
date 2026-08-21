#pragma once

#include "domain/model_types.h"
#include "domain/result.h"

#include <string>
#include <vector>

namespace agent {

struct ToolExecutionContext {
    std::string workspace_utf8;
};

class ToolGateway {
public:
    virtual ~ToolGateway() = default;
    virtual std::vector<ToolDefinition> definitions() const = 0;
    virtual Result<ToolResult> execute(
        const ToolCall& call,
        const ToolExecutionContext& context) = 0;
};

}  // namespace agent

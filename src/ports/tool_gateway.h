#pragma once

#include "domain/model_types.h"
#include "domain/result.h"

#include <vector>

namespace agent {

class ToolGateway {
public:
    virtual ~ToolGateway() = default;
    virtual std::vector<ToolDefinition> definitions() const = 0;
    virtual Result<ToolResult> execute(const ToolCall& call) = 0;
};

}  // namespace agent

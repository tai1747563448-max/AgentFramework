#pragma once

#include "ports/process_runner.h"
#include "ports/tool_gateway.h"

#include <cstdint>
#include <vector>

namespace agent {

class CMakeToolGateway final : public ToolGateway {
public:
    explicit CMakeToolGateway(ProcessRunner& process_runner,
                              std::int64_t timeout_ms);

    std::vector<ToolDefinition> definitions() const override;
    Result<ToolResult> execute(
        const ToolCall& call,
        const ToolExecutionContext& context) override;

private:
    ProcessRunner& process_runner_;
    std::int64_t timeout_ms_;
};

}  // namespace agent

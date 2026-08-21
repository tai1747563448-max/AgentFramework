#pragma once

#include "adapters/workspace/workspace_file_ops.h"
#include "ports/tool_gateway.h"

#include <filesystem>

namespace agent {

class WorkspaceToolGateway final : public ToolGateway {
public:
    explicit WorkspaceToolGateway(std::filesystem::path runtime_root);

    std::vector<ToolDefinition> definitions() const override;
    Result<ToolResult> execute(
        const ToolCall& call,
        const ToolExecutionContext& context) override;

private:
    workspace::WorkspacePathPolicy policy_;
    workspace::WorkspaceFileOps files_;
};

}  // namespace agent

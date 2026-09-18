#pragma once

#include "adapters/workspace/workspace_file_ops.h"
#include "ports/tool_gateway.h"

#include <filesystem>
#include <memory>
#include <unordered_map>

namespace agent {

// WorkspaceToolGateway exposes the five workspace tools via the T04
// build_tool() factory: each tool is a small Tool object with fail-closed
// defaults. read-only tools (list_files / read_file / search_text) opt in
// to concurrency safety so RuntimeEngine::AwaitingTool can dispatch them
// via std::async; the mutating tools (replace_text / write_file) keep the
// default false because their expected_sha256 guards would race under
// concurrent invocation.
class WorkspaceToolGateway final : public ToolGateway {
public:
    explicit WorkspaceToolGateway(std::filesystem::path runtime_root);

    std::vector<ToolDefinition> definitions() const override;
    Result<ToolResult> execute(
        const ToolCall& call,
        const ToolExecutionContext& context) override;

    bool tool_is_concurrency_safe(
        const std::string& name) const override;
    bool tool_is_read_only(
        const std::string& name) const override;

private:
    workspace::WorkspacePathPolicy policy_;
    workspace::WorkspaceFileOps files_;
    std::unordered_map<std::string, std::unique_ptr<Tool>> tools_;
};

}  // namespace agent

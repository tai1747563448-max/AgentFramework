#pragma once

#include "adapters/workspace/workspace_types.h"

#include <filesystem>
#include <string_view>

namespace agent::workspace {

class WorkspacePathPolicy {
public:
    explicit WorkspacePathPolicy(std::filesystem::path runtime_root);

    Outcome<RelativePath> parse(std::string_view text) const;
    Outcome<std::filesystem::path> resolve_existing(
        const std::filesystem::path& workspace,
        const RelativePath& relative,
        bool require_directory) const;
    Outcome<std::filesystem::path> resolve_parent(
        const std::filesystem::path& workspace,
        const RelativePath& relative) const;

private:
    bool is_protected(const std::filesystem::path& workspace,
                      const RelativePath& relative) const;

    std::filesystem::path runtime_root_;
};

}  // namespace agent::workspace

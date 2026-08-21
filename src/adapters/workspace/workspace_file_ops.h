#pragma once

#include "adapters/workspace/workspace_path_policy.h"
#include "adapters/workspace/workspace_types.h"

#include <cstddef>
#include <filesystem>
#include <string_view>

namespace agent::workspace {

class WorkspaceFileOps {
public:
    explicit WorkspaceFileOps(std::filesystem::path runtime_root);

    Outcome<ListOutput> list(const std::filesystem::path& workspace,
                             const RelativePath& path,
                             bool recursive,
                             std::size_t max_results) const;
    Outcome<ReadOutput> read(const std::filesystem::path& workspace,
                             const RelativePath& path,
                             std::size_t start_line,
                             std::size_t max_lines) const;
    Outcome<SearchOutput> search(const std::filesystem::path& workspace,
                                 const RelativePath& path,
                                 std::string_view query,
                                 bool case_sensitive,
                                 std::size_t max_results) const;

private:
    WorkspacePathPolicy policy_;
};

}  // namespace agent::workspace

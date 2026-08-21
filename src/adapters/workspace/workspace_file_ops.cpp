#include "adapters/workspace/workspace_file_ops.h"

#include "adapters/workspace/workspace_text.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace agent::workspace {
namespace {

constexpr std::uintmax_t kMaxTextFileBytes = 1024U * 1024U;
constexpr std::size_t kMaxTraversalEntries = 2000;
constexpr std::size_t kReadContentBudget = 60U * 1024U;

Fault io_fault() {
    return {FaultCode::IoError, "workspace file operation failed", false};
}

Fault file_limit_fault() {
    return {FaultCode::LimitExceeded,
            "workspace file exceeds the size limit", false};
}

Fault text_fault() {
    return {FaultCode::UnsupportedFile,
            "file is not supported UTF-8 text", false};
}

bool is_fault(const Outcome<std::filesystem::path>& outcome) {
    return std::holds_alternative<Fault>(outcome);
}

std::string relative_utf8(const std::filesystem::path& workspace,
                          const std::filesystem::path& candidate) {
    return candidate.lexically_relative(workspace).generic_u8string();
}

}  // namespace

WorkspaceFileOps::WorkspaceFileOps(std::filesystem::path runtime_root)
    : policy_(std::move(runtime_root)) {}

Outcome<ListOutput> WorkspaceFileOps::list(
    const std::filesystem::path& workspace,
    const RelativePath& path,
    bool recursive,
    std::size_t max_results) const {
    if (max_results == 0 || max_results > 200) {
        return Fault{FaultCode::InvalidArguments,
                     "invalid list_files arguments", false};
    }
    const auto resolved = policy_.resolve_existing(workspace, path, true);
    if (is_fault(resolved)) {
        return std::get<Fault>(resolved);
    }
    std::error_code error;
    const auto workspace_root = std::filesystem::weakly_canonical(
        std::filesystem::absolute(workspace, error), error);
    if (error) {
        return io_fault();
    }

    std::vector<std::filesystem::path> pending{
        std::get<std::filesystem::path>(resolved)};
    ListOutput output;
    std::size_t inspected = 0;
    while (!pending.empty()) {
        const auto directory = std::move(pending.back());
        pending.pop_back();
        std::vector<std::filesystem::path> children;
        std::filesystem::directory_iterator iterator(directory, error);
        if (error) {
            return io_fault();
        }
        for (const auto& entry : iterator) {
            children.push_back(entry.path());
        }
        if (error) {
            return io_fault();
        }
        std::sort(children.begin(), children.end(),
                  [&](const auto& left, const auto& right) {
                      return relative_utf8(workspace_root, left) <
                             relative_utf8(workspace_root, right);
                  });
        for (const auto& child : children) {
            if (inspected >= kMaxTraversalEntries) {
                output.truncated = true;
                output.truncation_reason = "entry_budget";
                pending.clear();
                break;
            }
            ++inspected;
            const auto logical_text = relative_utf8(workspace_root, child);
            const auto logical = policy_.parse(logical_text);
            if (std::holds_alternative<Fault>(logical)) {
                ++output.omitted_entries;
                continue;
            }
            const auto& relative = std::get<RelativePath>(logical);
            const auto status = std::filesystem::symlink_status(child, error);
            if (error) {
                ++output.omitted_entries;
                error.clear();
                continue;
            }
            if (std::filesystem::is_directory(status)) {
                const auto allowed =
                    policy_.resolve_existing(workspace_root, relative, true);
                if (std::holds_alternative<Fault>(allowed)) {
                    ++output.omitted_entries;
                    continue;
                }
                output.entries.push_back({logical_text, true, 0});
                if (recursive) {
                    pending.push_back(child);
                }
            } else if (std::filesystem::is_regular_file(status)) {
                const auto allowed =
                    policy_.resolve_existing(workspace_root, relative, false);
                if (std::holds_alternative<Fault>(allowed)) {
                    ++output.omitted_entries;
                    continue;
                }
                const auto size = std::filesystem::file_size(child, error);
                if (error) {
                    ++output.omitted_entries;
                    error.clear();
                    continue;
                }
                output.entries.push_back({logical_text, false, size});
            } else {
                ++output.omitted_entries;
            }
        }
    }
    std::sort(output.entries.begin(), output.entries.end(),
              [](const auto& left, const auto& right) {
                  return left.path < right.path;
              });
    if (output.entries.size() > max_results) {
        output.entries.resize(max_results);
        output.truncated = true;
        output.truncation_reason = "max_results";
    }
    return output;
}

Outcome<ReadOutput> WorkspaceFileOps::read(
    const std::filesystem::path& workspace,
    const RelativePath& path,
    std::size_t start_line,
    std::size_t max_lines) const {
    if (start_line == 0 || max_lines == 0 || max_lines > 1000) {
        return Fault{FaultCode::InvalidArguments,
                     "invalid read_file arguments", false};
    }
    const auto resolved = policy_.resolve_existing(workspace, path, false);
    if (std::holds_alternative<Fault>(resolved)) {
        return std::get<Fault>(resolved);
    }
    const auto& file = std::get<std::filesystem::path>(resolved);
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    if (error) {
        return io_fault();
    }
    if (size > kMaxTextFileBytes) {
        return file_limit_fault();
    }
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        return io_fault();
    }
    std::string bytes{std::istreambuf_iterator<char>(input),
                      std::istreambuf_iterator<char>()};
    if (!input.eof() && input.fail()) {
        return io_fault();
    }
    if (bytes.size() != static_cast<std::size_t>(size)) {
        return io_fault();
    }
    if (!is_strict_utf8_text(bytes)) {
        return text_fault();
    }
    auto page = line_page(bytes, start_line, max_lines, kReadContentBudget);
    if (std::holds_alternative<Fault>(page)) {
        return std::get<Fault>(page);
    }
    return ReadOutput{path.generic, sha256_hex(bytes),
                      std::get<LinePage>(std::move(page))};
}

}  // namespace agent::workspace

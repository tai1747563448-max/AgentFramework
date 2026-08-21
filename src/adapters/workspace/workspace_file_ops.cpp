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
constexpr std::size_t kMaxSearchFiles = 500;
constexpr std::uintmax_t kMaxSearchBytes = 16U * 1024U * 1024U;
constexpr std::size_t kReadContentBudget = 60U * 1024U;
constexpr std::size_t kMaxLineFragmentBytes = 1024;

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

struct SearchCandidate {
    std::string path;
    std::filesystem::path absolute;
};

Outcome<std::string> load_text_file(const std::filesystem::path& file) {
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
    return bytes;
}

std::size_t utf8_column(std::string_view line, std::size_t byte_offset) {
    std::size_t column = 1;
    for (std::size_t index = 0; index < byte_offset; ++index) {
        const auto byte = static_cast<unsigned char>(line[index]);
        if ((byte & 0xC0U) != 0x80U) {
            ++column;
        }
    }
    return column;
}

bool continuation_byte(char value) {
    return (static_cast<unsigned char>(value) & 0xC0U) == 0x80U;
}

std::pair<std::string, bool> line_fragment(std::string_view line,
                                           std::size_t match_offset,
                                           std::size_t query_bytes) {
    if (line.size() <= kMaxLineFragmentBytes) {
        return {std::string(line), false};
    }
    std::size_t begin = match_offset > kMaxLineFragmentBytes / 3
                            ? match_offset - kMaxLineFragmentBytes / 3
                            : 0;
    std::size_t end = std::min(line.size(), begin + kMaxLineFragmentBytes);
    if (match_offset + query_bytes > end) {
        end = std::min(line.size(), match_offset + query_bytes);
        begin = end > kMaxLineFragmentBytes
                    ? end - kMaxLineFragmentBytes
                    : 0;
    }
    while (begin < end && continuation_byte(line[begin])) {
        ++begin;
    }
    while (end > begin && end < line.size() && continuation_byte(line[end])) {
        --end;
    }
    return {std::string(line.substr(begin, end - begin)), true};
}

void mark_truncated(SearchOutput& output, std::string reason) {
    if (!output.truncated) {
        output.truncated = true;
        output.truncation_reason = std::move(reason);
    }
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
    auto loaded = load_text_file(file);
    if (std::holds_alternative<Fault>(loaded)) {
        return std::get<Fault>(loaded);
    }
    auto bytes = std::get<std::string>(std::move(loaded));
    auto page = line_page(bytes, start_line, max_lines, kReadContentBudget);
    if (std::holds_alternative<Fault>(page)) {
        return std::get<Fault>(page);
    }
    return ReadOutput{path.generic, sha256_hex(bytes),
                      std::get<LinePage>(std::move(page))};
}

Outcome<SearchOutput> WorkspaceFileOps::search(
    const std::filesystem::path& workspace,
    const RelativePath& path,
    std::string_view query,
    bool case_sensitive,
    std::size_t max_results) const {
    if (query.empty() || !is_strict_utf8_text(query) || max_results == 0 ||
        max_results > 200) {
        return Fault{FaultCode::InvalidArguments,
                     "invalid search_text arguments", false};
    }

    const auto directory = policy_.resolve_existing(workspace, path, true);
    const bool direct_file = std::holds_alternative<Fault>(directory);
    std::vector<SearchCandidate> candidates;
    SearchOutput output;
    if (direct_file) {
        const auto& directory_fault = std::get<Fault>(directory);
        if (directory_fault.code != FaultCode::UnsupportedFile) {
            return directory_fault;
        }
        const auto file = policy_.resolve_existing(workspace, path, false);
        if (std::holds_alternative<Fault>(file)) {
            return std::get<Fault>(file);
        }
        candidates.push_back(
            {path.generic, std::get<std::filesystem::path>(file)});
    } else {
        std::error_code error;
        const auto root = std::filesystem::weakly_canonical(
            std::filesystem::absolute(workspace, error), error);
        if (error) {
            return io_fault();
        }
        std::vector<std::filesystem::path> pending{
            std::get<std::filesystem::path>(directory)};
        std::size_t next_directory = 0;
        std::size_t inspected = 0;
        bool entry_budget_reached = false;
        while (next_directory < pending.size() && !entry_budget_reached) {
            const auto current = pending[next_directory++];
            std::vector<std::filesystem::path> children;
            std::filesystem::directory_iterator iterator(current, error);
            if (error) {
                return io_fault();
            }
            const std::filesystem::directory_iterator end;
            while (iterator != end) {
                children.push_back(iterator->path());
                iterator.increment(error);
                if (error) {
                    return io_fault();
                }
            }
            std::sort(children.begin(), children.end(),
                      [&](const auto& left, const auto& right) {
                          return relative_utf8(root, left) <
                                 relative_utf8(root, right);
                      });
            for (const auto& child : children) {
                if (inspected >= kMaxTraversalEntries) {
                    entry_budget_reached = true;
                    mark_truncated(output, "entry_budget");
                    break;
                }
                ++inspected;
                const auto logical_text = relative_utf8(root, child);
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
                        policy_.resolve_existing(root, relative, true);
                    if (std::holds_alternative<Fault>(allowed)) {
                        ++output.omitted_entries;
                        continue;
                    }
                    pending.push_back(
                        std::get<std::filesystem::path>(allowed));
                } else if (std::filesystem::is_regular_file(status)) {
                    const auto allowed =
                        policy_.resolve_existing(root, relative, false);
                    if (std::holds_alternative<Fault>(allowed)) {
                        ++output.omitted_entries;
                        continue;
                    }
                    candidates.push_back(
                        {logical_text,
                         std::get<std::filesystem::path>(allowed)});
                } else {
                    ++output.omitted_entries;
                }
            }
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const auto& left, const auto& right) {
                  return left.path < right.path;
              });
    std::uintmax_t scanned_bytes = 0;
    for (const auto& candidate : candidates) {
        if (output.scanned_files >= kMaxSearchFiles) {
            mark_truncated(output, "file_budget");
            break;
        }
        std::error_code error;
        const auto size = std::filesystem::file_size(candidate.absolute, error);
        if (error || size > kMaxTextFileBytes) {
            if (direct_file) {
                return error ? io_fault() : file_limit_fault();
            }
            ++output.omitted_entries;
            continue;
        }
        if (size > kMaxSearchBytes - scanned_bytes) {
            mark_truncated(output, "byte_budget");
            break;
        }
        scanned_bytes += size;
        ++output.scanned_files;
        auto loaded = load_text_file(candidate.absolute);
        if (std::holds_alternative<Fault>(loaded)) {
            if (direct_file) {
                return std::get<Fault>(loaded);
            }
            ++output.omitted_entries;
            continue;
        }
        const auto& text = std::get<std::string>(loaded);
        std::size_t line_number = 1;
        std::size_t begin = 0;
        while (begin < text.size()) {
            const auto newline = text.find('\n', begin);
            auto line_end = newline == std::string::npos ? text.size() : newline;
            if (line_end > begin && text[line_end - 1] == '\r') {
                --line_end;
            }
            const std::string_view line(text.data() + begin, line_end - begin);
            for (const auto byte_offset :
                 literal_matches(line, query, case_sensitive)) {
                if (output.matches.size() >= max_results) {
                    mark_truncated(output, "max_results");
                    return output;
                }
                auto fragment =
                    line_fragment(line, byte_offset, query.size());
                output.matches.push_back(
                    {candidate.path, line_number,
                     utf8_column(line, byte_offset),
                     std::move(fragment.first), fragment.second});
            }
            if (newline == std::string::npos) {
                break;
            }
            begin = newline + 1;
            ++line_number;
        }
    }
    return output;
}

}  // namespace agent::workspace

#include "adapters/workspace/workspace_file_ops.h"

#include "adapters/workspace/workspace_text.h"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <stdexcept>
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

Fault conflict_fault() {
    return {FaultCode::Conflict,
            "file content changed; read the file again", true};
}

Fault denied_fault() {
    return {FaultCode::AccessDenied,
            "workspace path is not permitted", false};
}

Fault not_found_fault() {
    return {FaultCode::NotFound, "workspace path was not found", false};
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

bool valid_sha256(std::string_view hash) {
    return hash.size() == 64 &&
           std::all_of(hash.begin(), hash.end(), [](unsigned char value) {
               return (value >= static_cast<unsigned char>('0') &&
                       value <= static_cast<unsigned char>('9')) ||
                      (value >= static_cast<unsigned char>('a') &&
                       value <= static_cast<unsigned char>('f'));
           });
}

std::string temp_leaf_name() {
    static std::atomic<std::uint64_t> counter{0};
    std::random_device random;
    const auto stamp = std::chrono::high_resolution_clock::now()
                           .time_since_epoch()
                           .count();
    return ".agent-tmp-" + std::to_string(stamp) + "-" +
           std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) +
           "-" + std::to_string(random());
}

class TempPathGuard {
public:
    explicit TempPathGuard(std::filesystem::path path)
        : path_(std::move(path)) {}

    ~TempPathGuard() {
        if (active_) {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    }

    void release() noexcept {
        active_ = false;
    }

private:
    std::filesystem::path path_;
    bool active_{true};
};

Outcome<bool> write_exclusive(const std::filesystem::path& path,
                              std::string_view content) {
#if defined(_WIN32)
    const HANDLE handle = CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
            return conflict_fault();
        }
        return io_fault();
    }
    bool okay = true;
    std::size_t written = 0;
    while (written < content.size()) {
        DWORD chunk_written = 0;
        const auto remaining = content.size() - written;
        const auto chunk = static_cast<DWORD>(
            std::min<std::size_t>(remaining,
                                  std::numeric_limits<DWORD>::max()));
        if (!WriteFile(handle, content.data() + written, chunk,
                       &chunk_written, nullptr) || chunk_written == 0) {
            okay = false;
            break;
        }
        written += chunk_written;
    }
    if (okay && !FlushFileBuffers(handle)) {
        okay = false;
    }
    if (!CloseHandle(handle)) {
        okay = false;
    }
    if (!okay) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    return okay ? Outcome<bool>{true} : Outcome<bool>{io_fault()};
#else
    const int descriptor =
        ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (descriptor < 0) {
        return errno == EEXIST ? Outcome<bool>{conflict_fault()}
                              : Outcome<bool>{io_fault()};
    }
    bool okay = true;
    std::size_t written = 0;
    while (written < content.size()) {
        const auto result =
            ::write(descriptor, content.data() + written,
                    content.size() - written);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            okay = false;
            break;
        }
        written += static_cast<std::size_t>(result);
    }
    if (okay && ::fsync(descriptor) != 0) {
        okay = false;
    }
    if (::close(descriptor) != 0) {
        okay = false;
    }
    if (!okay) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    return okay ? Outcome<bool>{true} : Outcome<bool>{io_fault()};
#endif
}

Outcome<bool> trustworthy_overwrite_target(
    const std::filesystem::path& target,
    std::string_view expected_sha256) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(target, error);
    if (error) {
        return io_fault();
    }
    if (!std::filesystem::exists(status)) {
        return not_found_fault();
    }
    if (std::filesystem::is_symlink(status) ||
        !std::filesystem::is_regular_file(status)) {
        return denied_fault();
    }
#if defined(_WIN32)
    const DWORD attributes = GetFileAttributesW(target.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return io_fault();
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return denied_fault();
    }
#endif
    const auto links = std::filesystem::hard_link_count(target, error);
    if (error) {
        return io_fault();
    }
    if (links != 1) {
        return denied_fault();
    }
    const auto current = load_text_file(target);
    if (std::holds_alternative<Fault>(current)) {
        return std::get<Fault>(current);
    }
    if (sha256_hex(std::get<std::string>(current)) != expected_sha256) {
        return conflict_fault();
    }
    return true;
}

Outcome<bool> install_file(const std::filesystem::path& target,
                           std::string_view content,
                           bool create,
                           std::optional<std::string_view> expected_sha256) {
    std::filesystem::path temporary;
    Outcome<bool> created = conflict_fault();
    for (std::size_t attempt = 0; attempt < 32; ++attempt) {
        temporary = target.parent_path() /
                    std::filesystem::u8path(temp_leaf_name());
        created = write_exclusive(temporary, content);
        if (std::holds_alternative<bool>(created)) {
            break;
        }
        const auto& fault = std::get<Fault>(created);
        if (fault.code != FaultCode::Conflict) {
            return fault;
        }
    }
    if (std::holds_alternative<Fault>(created)) {
        return io_fault();
    }
    TempPathGuard cleanup(temporary);
    const auto temporary_bytes = load_text_file(temporary);
    if (std::holds_alternative<Fault>(temporary_bytes) ||
        sha256_hex(std::get<std::string>(temporary_bytes)) !=
            sha256_hex(content)) {
        return io_fault();
    }
    if (!create) {
        if (!expected_sha256.has_value()) {
            return conflict_fault();
        }
        const auto trustworthy =
            trustworthy_overwrite_target(target, *expected_sha256);
        if (std::holds_alternative<Fault>(trustworthy)) {
            return std::get<Fault>(trustworthy);
        }
#if !defined(_WIN32)
        std::error_code error;
        const auto permissions =
            std::filesystem::status(target, error).permissions();
        if (error) {
            return io_fault();
        }
        std::filesystem::permissions(
            temporary, permissions,
            std::filesystem::perm_options::replace, error);
        if (error) {
            return io_fault();
        }
#endif
    }

#if defined(_WIN32)
    DWORD flags = MOVEFILE_WRITE_THROUGH;
    if (!create) {
        flags |= MOVEFILE_REPLACE_EXISTING;
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(), flags)) {
        const auto error = GetLastError();
        if (create &&
            (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)) {
            return conflict_fault();
        }
        return io_fault();
    }
    cleanup.release();
#else
    if (create) {
        if (::link(temporary.c_str(), target.c_str()) != 0) {
            return errno == EEXIST ? Outcome<bool>{conflict_fault()}
                                  : Outcome<bool>{io_fault()};
        }
        if (::unlink(temporary.c_str()) != 0) {
            throw std::runtime_error("installed file state is not trustworthy");
        }
        cleanup.release();
    } else {
        std::error_code error;
        std::filesystem::rename(temporary, target, error);
        if (error) {
            return io_fault();
        }
        cleanup.release();
    }
#endif

    const auto installed = load_text_file(target);
    if (std::holds_alternative<Fault>(installed) ||
        sha256_hex(std::get<std::string>(installed)) != sha256_hex(content)) {
        throw std::runtime_error("installed file state is not trustworthy");
    }
    return true;
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

Outcome<WriteOutput> WorkspaceFileOps::replace_text(
    const std::filesystem::path& workspace,
    const RelativePath& path,
    std::string_view old_text,
    std::string_view new_text,
    std::size_t expected_occurrences,
    std::string_view expected_sha256) const {
    if (old_text.empty() || !is_strict_utf8_text(old_text) ||
        !is_strict_utf8_text(new_text) || expected_occurrences == 0 ||
        expected_occurrences > 1000 || !valid_sha256(expected_sha256)) {
        return Fault{FaultCode::InvalidArguments,
                     "invalid replace_text arguments", false};
    }
    if (new_text.size() > kMaxTextFileBytes) {
        return file_limit_fault();
    }
    const auto resolved = policy_.resolve_existing(workspace, path, false);
    if (std::holds_alternative<Fault>(resolved)) {
        return std::get<Fault>(resolved);
    }
    const auto& target = std::get<std::filesystem::path>(resolved);
    auto loaded = load_text_file(target);
    if (std::holds_alternative<Fault>(loaded)) {
        return std::get<Fault>(loaded);
    }
    const auto& current = std::get<std::string>(loaded);
    const auto current_hash = sha256_hex(current);
    if (current_hash != expected_sha256) {
        return conflict_fault();
    }
    const auto positions = literal_matches(current, old_text, true);
    if (positions.size() != expected_occurrences) {
        return conflict_fault();
    }

    std::string replacement;
    replacement.reserve(current.size());
    std::size_t copied = 0;
    for (const auto position : positions) {
        replacement.append(current, copied, position - copied);
        replacement.append(new_text);
        if (replacement.size() > kMaxTextFileBytes) {
            return file_limit_fault();
        }
        copied = position + old_text.size();
    }
    replacement.append(current, copied, std::string::npos);
    if (replacement.size() > kMaxTextFileBytes) {
        return file_limit_fault();
    }
    const auto installed = install_file(
        target, replacement, false,
        std::optional<std::string_view>{expected_sha256});
    if (std::holds_alternative<Fault>(installed)) {
        return std::get<Fault>(installed);
    }
    return WriteOutput{path.generic,
                       false,
                       current_hash,
                       sha256_hex(replacement),
                       positions.size(),
                       replacement.size()};
}

Outcome<WriteOutput> WorkspaceFileOps::write_file(
    const std::filesystem::path& workspace,
    const RelativePath& path,
    std::string_view content,
    bool create,
    std::optional<std::string_view> expected_sha256) const {
    if (!is_strict_utf8_text(content) || path.components.empty() ||
        (create && expected_sha256.has_value()) ||
        (!create && (!expected_sha256.has_value() ||
                     !valid_sha256(*expected_sha256)))) {
        return Fault{FaultCode::InvalidArguments,
                     "invalid write_file arguments", false};
    }
    if (content.size() > kMaxTextFileBytes) {
        return file_limit_fault();
    }
    if (create) {
        const auto parent = policy_.resolve_parent(workspace, path);
        if (std::holds_alternative<Fault>(parent)) {
            return std::get<Fault>(parent);
        }
        const auto target = std::get<std::filesystem::path>(parent) /
                            std::filesystem::u8path(path.components.back());
        std::error_code error;
        if (std::filesystem::exists(target, error)) {
            return conflict_fault();
        }
        if (error) {
            return io_fault();
        }
        const auto installed =
            install_file(target, content, true, std::nullopt);
        if (std::holds_alternative<Fault>(installed)) {
            return std::get<Fault>(installed);
        }
        return WriteOutput{path.generic,
                           true,
                           "",
                           sha256_hex(content),
                           0,
                           content.size()};
    }

    const auto resolved = policy_.resolve_existing(workspace, path, false);
    if (std::holds_alternative<Fault>(resolved)) {
        return std::get<Fault>(resolved);
    }
    const auto& target = std::get<std::filesystem::path>(resolved);
    const auto loaded = load_text_file(target);
    if (std::holds_alternative<Fault>(loaded)) {
        return std::get<Fault>(loaded);
    }
    const auto old_hash = sha256_hex(std::get<std::string>(loaded));
    if (old_hash != *expected_sha256) {
        return conflict_fault();
    }
    const auto installed =
        install_file(target, content, false, expected_sha256);
    if (std::holds_alternative<Fault>(installed)) {
        return std::get<Fault>(installed);
    }
    return WriteOutput{path.generic,
                       false,
                       old_hash,
                       sha256_hex(content),
                       0,
                       content.size()};
}

}  // namespace agent::workspace

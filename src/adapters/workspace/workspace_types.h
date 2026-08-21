#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace agent::workspace {

enum class FaultCode {
    InvalidArguments,
    AccessDenied,
    NotFound,
    Conflict,
    UnsupportedFile,
    LimitExceeded,
    IoError
};

struct Fault {
    FaultCode code;
    std::string message;
    bool retryable{false};
};

template <typename T>
using Outcome = std::variant<T, Fault>;

struct RelativePath {
    std::string generic;
    std::vector<std::string> components;
};

struct LinePage {
    std::size_t start_line{1};
    std::size_t end_line{0};
    std::size_t total_lines{0};
    std::string content;
    std::optional<std::size_t> next_start_line;
};

struct WorkspaceEntry {
    std::string path;
    bool directory{false};
    std::uintmax_t size_bytes{0};
};

struct ListOutput {
    std::vector<WorkspaceEntry> entries;
    bool truncated{false};
    std::string truncation_reason;
    std::size_t omitted_entries{0};
};

struct ReadOutput {
    std::string path;
    std::string sha256;
    LinePage page;
};

struct SearchMatch {
    std::string path;
    std::size_t line{0};
    std::size_t column{0};
    std::string text;
    bool line_truncated{false};
};

struct SearchOutput {
    std::vector<SearchMatch> matches;
    bool truncated{false};
    std::string truncation_reason;
    std::size_t scanned_files{0};
    std::size_t omitted_entries{0};
};

struct WriteOutput {
    std::string path;
    bool created{false};
    std::string old_sha256;
    std::string new_sha256;
    std::size_t replacements{0};
    std::size_t bytes_written{0};
};

}  // namespace agent::workspace

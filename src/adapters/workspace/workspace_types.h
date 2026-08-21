#pragma once

#include <cstddef>
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

}  // namespace agent::workspace

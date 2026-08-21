#pragma once

#include "domain/result.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace agent {

struct ProcessRequest {
    std::string program;
    std::vector<std::string> arguments;
    std::filesystem::path working_directory;
    std::string stdin_utf8;
    std::int64_t timeout_ms{300'000};
    std::size_t max_stdout_bytes{64 * 1024};
    std::size_t max_stderr_bytes{64 * 1024};
    std::vector<std::pair<std::string, std::string>> environment_overrides;
};

struct ProcessOutput {
    std::int64_t exit_code{0};
    bool timed_out{false};
    std::int64_t duration_ms{0};
    std::string stdout_utf8;
    std::string stderr_utf8;
    bool stdout_truncated{false};
    bool stderr_truncated{false};
};

class ProcessRunner {
public:
    virtual ~ProcessRunner() = default;
    virtual Result<ProcessOutput> run(const ProcessRequest& request) = 0;
};

}  // namespace agent

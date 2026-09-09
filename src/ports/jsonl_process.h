#pragma once

#include "domain/result.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace agent {

struct JsonlProcessRequest {
    std::string program;
    std::vector<std::string> arguments;
    std::filesystem::path working_directory;
    std::int64_t startup_timeout_ms{120'000};
    std::size_t max_stdout_line_bytes{65'536};
    std::size_t max_stderr_bytes{4'096};
};

class JsonlProcess {
public:
    virtual ~JsonlProcess() = default;
    virtual Result<std::string> start(const JsonlProcessRequest& request) = 0;
    virtual Result<std::string> exchange(const std::string& line,
                                         std::int64_t timeout_ms) = 0;
    virtual bool running() const noexcept = 0;
    virtual Result<void> stop(std::int64_t timeout_ms) = 0;
};

}  // namespace agent

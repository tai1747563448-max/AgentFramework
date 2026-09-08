#pragma once

#include "ports/jsonl_process.h"

#include <memory>
#include <functional>
#include <string>

namespace agent {

class ReprocJsonlProcess final : public JsonlProcess {
public:
    ReprocJsonlProcess();
    explicit ReprocJsonlProcess(
        std::function<void(const std::string&)> progress_observer);
    ~ReprocJsonlProcess() override;
    ReprocJsonlProcess(const ReprocJsonlProcess&) = delete;
    ReprocJsonlProcess& operator=(const ReprocJsonlProcess&) = delete;
    ReprocJsonlProcess(ReprocJsonlProcess&&) = delete;
    ReprocJsonlProcess& operator=(ReprocJsonlProcess&&) = delete;

    Result<std::string> start(const JsonlProcessRequest& request) override;
    Result<std::string> exchange(const std::string& line,
                                 std::int64_t timeout_ms) override;
    bool running() const noexcept override;
    Result<void> stop(std::int64_t timeout_ms) override;

    std::string stderr_tail() const;
    bool stderr_truncated() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace agent

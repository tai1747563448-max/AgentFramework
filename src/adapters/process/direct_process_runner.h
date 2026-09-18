#pragma once

#include "ports/process_runner.h"

#include <memory>
#include <string>

namespace agent {

class Sandbox;

class DirectProcessRunner final : public ProcessRunner {
public:
    DirectProcessRunner();
    // T22: pass a sandbox to wrap every process invocation. When the
    // pointer is null the runner degrades to its historical behaviour
    // (no sandboxing). Callers usually pass a SandboxFactory output
    // whose name() describes the active backend ("bwrap", "seatbelt",
    // "job_object", or "none").
    explicit DirectProcessRunner(std::shared_ptr<Sandbox> sandbox);
    Result<ProcessOutput> run(const ProcessRequest& request) override;

    // Read-only accessor used by diagnostics and the audit hook (T13)
    // to record which sandbox was active for the call.
    const Sandbox* sandbox() const noexcept { return sandbox_.get(); }

private:
    std::shared_ptr<Sandbox> sandbox_;
};

}  // namespace agent

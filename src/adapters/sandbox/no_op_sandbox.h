#pragma once

#include "ports/sandbox.h"

#include <string>

namespace agent {

// T22: no-op sandbox used when sandboxing is disabled or unavailable on
// the host platform. Always succeeds at apply(); the program path passes
// through unchanged. The runner does not consume any extra resource when
// this adapter is selected.
class NoOpSandbox final : public Sandbox {
public:
    std::optional<SandboxFault> validate_profile(
        const SandboxProfile& profile) const override;
    std::optional<SandboxFault> apply(
        const SandboxProfile& profile,
        std::string_view program_path,
        const std::vector<std::string>& arguments,
        std::string_view working_directory,
        SandboxedCommand& out) const override;
    std::string name() const override { return "none"; }
};

}  // namespace agent

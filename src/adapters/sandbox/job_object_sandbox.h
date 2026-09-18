#pragma once

#include "ports/sandbox.h"

#include <string>

namespace agent {

// T22: Windows Job Object adapter. Translates a SandboxProfile into the
// JOBOBJECT_EXTENDED_LIMIT_INFORMATION fields the runner applies via
// SetInformationJobObject after CreateProcessW. Network restriction on
// Windows requires the heavier firewall / WFP APIs and is *not* enforced
// by this adapter - the profile field is preserved so callers can layer
// a separate firewall rule if needed.
class JobObjectSandbox final : public Sandbox {
public:
    JobObjectSandbox();
    std::optional<SandboxFault> validate_profile(
        const SandboxProfile& profile) const override;
    std::optional<SandboxFault> apply(
        const SandboxProfile& profile,
        std::string_view program_path,
        const std::vector<std::string>& arguments,
        std::string_view working_directory,
        SandboxedCommand& out) const override;
    std::string name() const override { return "job_object"; }

    // Build the comma-separated Job Object limit flags a Windows host can
    // pass to SetInformationJobObject. Returned as a diagnostic string;
    // DirectProcessRunner reads the same fields via the dedicated limit
    // struct it owns.
    std::string describe_limits(const SandboxProfile& profile) const;
};

}  // namespace agent

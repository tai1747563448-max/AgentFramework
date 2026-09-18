#pragma once

#include "ports/sandbox.h"

#include <string>

namespace agent {

// T22: Linux bubblewrap adapter. Translates a SandboxProfile into the
// canonical bwrap(1) flag set:
//
//   --unshare-all-try                  # start a fresh mount + PID + net ns
//   --die-with-parent                  # kill child if parent dies
//   --ro-bind <src> <dst> ...          # read-only mounts
//   --bind <src> <dst> ...             # read-write mounts
//   --bind-data <data> <path>          # bind /etc/{passwd,group,resolv.conf}
//   --tmpfs <path>                     # ephemeral tmpfs at /tmp etc.
//   --proc /proc                       # minimal /proc
//   --dev /dev                         # minimal /dev
//   --chdir <workspace>                # enter workspace
//   -- <argv...>                       # exec the program
//
// The adapter returns SandboxFaultCode::DependencyUnavailable when bwrap
// cannot be located on PATH; SandboxFaultCode::InvalidConfiguration when
// the profile references paths that don't exist on the host.
class BubblewrapSandbox final : public Sandbox {
public:
    BubblewrapSandbox();
    std::optional<SandboxFault> validate_profile(
        const SandboxProfile& profile) const override;
    std::optional<SandboxFault> apply(
        const SandboxProfile& profile,
        std::string_view program_path,
        const std::vector<std::string>& arguments,
        std::string_view working_directory,
        SandboxedCommand& out) const override;
    std::string name() const override { return "bwrap"; }
};

// Locate the bwrap binary on PATH. Empty string means "not found".
std::string locate_bubblewrap_binary();

}  // namespace agent

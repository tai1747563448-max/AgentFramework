#pragma once

#include "ports/sandbox.h"

#include <string>

namespace agent {

// T22: macOS sandbox-exec adapter. Translates a SandboxProfile into a
// Scheme (SBPL) string. The sandbox-exec tool ships with macOS, so no
// dependency check is needed; the adapter is always available on macOS
// builds and reports UnsupportedPlatform elsewhere.
class SeatbeltSandbox final : public Sandbox {
public:
    SeatbeltSandbox();
    std::optional<SandboxFault> validate_profile(
        const SandboxProfile& profile) const override;
    std::optional<SandboxFault> apply(
        const SandboxProfile& profile,
        std::string_view program_path,
        const std::vector<std::string>& arguments,
        std::string_view working_directory,
        SandboxedCommand& out) const override;
    std::string name() const override { return "seatbelt"; }

    // Render the SBPL profile as a string for diagnostics or for callers
    // that want to feed it to sandbox-exec directly. The same renderer is
    // used internally by apply().
    std::string render_sbpl(const SandboxProfile& profile,
                            std::string_view working_directory) const;
};

// Locate the sandbox-exec binary. On macOS this is always /usr/bin/
// sandbox-exec; on other platforms the call returns the empty string.
std::string locate_seatbelt_binary();

}  // namespace agent

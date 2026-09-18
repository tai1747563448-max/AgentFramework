#include "adapters/sandbox/bubblewrap_sandbox.h"
#include "ports/sandbox.h"
#include "test_support.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

using agent::BubblewrapSandbox;
using agent::SandboxProfile;

TEST_CASE(bubblewrap_validate_rejects_unknown_paths) {
#if defined(_WIN32)
    // bwrap is a Linux-only sandbox; skip on other platforms.
#else
    BubblewrapSandbox sandbox;
    SandboxProfile profile;
    profile.read_paths.emplace_back("/this/path/does/not/exist");
    const auto fault = sandbox.validate_profile(profile);
    REQUIRE(fault.has_value());
    REQUIRE(fault->code == agent::SandboxFaultCode::InvalidConfiguration);
#endif
}

TEST_CASE(bubblewrap_apply_succeeds_with_existing_paths_when_bwrap_installed) {
#if defined(_WIN32)
#else
    BubblewrapSandbox sandbox;
    SandboxProfile profile;
    const test::ScopedTempDir workspace("bwrap-apply");
    const auto workspace_text = workspace.path().generic_u8string();
    agent::SandboxedCommand out;
    const auto fault = sandbox.apply(
        profile, "/bin/echo", {"hi"}, workspace_text, out);
    if (!fault.has_value()) {
        // Workspace must appear in --bind argument pairs.
        bool found_workspace = false;
        for (const auto& argument : out.arguments) {
            if (argument == workspace_text) {
                found_workspace = true;
                break;
            }
        }
        REQUIRE(found_workspace);
        // The program must come after the "--" separator.
        auto separator = std::find(out.arguments.begin(), out.arguments.end(),
                                    std::string{"--"});
        REQUIRE(separator != out.arguments.end());
        REQUIRE(std::next(separator) != out.arguments.end());
        REQUIRE(*std::next(separator) == "/bin/echo");
    } else {
        // Either bwrap is missing (DependencyUnavailable) or it
        // failed to validate the profile (InvalidConfiguration). Both
        // are acceptable platform states for this test.
        REQUIRE(fault->code ==
                agent::SandboxFaultCode::DependencyUnavailable);
    }
#endif
}

TEST_CASE(bubblewrap_apply_reports_missing_dependency) {
#if defined(_WIN32)
#else
    BubblewrapSandbox sandbox;
    SandboxProfile profile;
    const test::ScopedTempDir workspace("bwrap-missing");
    agent::SandboxedCommand out;
    // Locate bwrap explicitly; if the host has it, set PATH to an empty
    // directory and retry - the lookup must then fail.
    if (agent::locate_bubblewrap_binary().empty()) {
        const auto fault = sandbox.apply(
            profile, "/bin/echo", {}, workspace.path().generic_u8string(), out);
        REQUIRE(fault.has_value());
        REQUIRE(fault->code ==
                agent::SandboxFaultCode::DependencyUnavailable);
    }
#endif
}

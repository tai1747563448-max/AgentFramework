#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agent {

// T22 (v2 §3): declarative sandbox profile. Default fields deny network
// access and writes outside the workspace; explicit allow lists override.
// The profile is the single source of truth consumed by every platform
// implementation; no platform-specific knobs leak through this header.
struct SandboxProfile {
    bool deny_network{true};
    bool deny_writes_outside_workspace{true};
    // Allow-list of read-only mount paths (relative or absolute). Empty
    // means "only the workspace is visible to read paths".
    std::vector<std::string> read_paths;
    // Allow-list of additional writable paths beyond the workspace.
    std::vector<std::string> write_paths;
    // Optional wall-clock CPU budget forwarded to the platform sandbox.
    std::optional<std::int64_t> max_cpu_time_ms;
    // Optional memory ceiling forwarded to the platform sandbox.
    std::optional<std::size_t> max_memory_bytes;
};

// Outcome of evaluating a request against a SandboxProfile. Distinct from
// the workspace Fault enum because the fault category is different (OS-level
// isolation rather than filesystem path validation).
enum class SandboxFaultCode {
    InvalidConfiguration,
    UnsupportedPlatform,
    DependencyUnavailable,
    LimitExceeded
};

struct SandboxFault {
    SandboxFaultCode code;
    std::string message;
};

// Plain-data description of a sandboxed command. Platform adapters translate
// this into their native invocation; DirectProcessRunner consumes the
// translated version.
struct SandboxedCommand {
    // Absolute path to the executable after wrapping (e.g. "bwrap" for
    // Linux, "sandbox-exec" for macOS). Empty when the host invokes the
    // program directly without a sandbox wrapper.
    std::string program;
    // Argument vector to pass to execvp / CreateProcessW. argv[0] is the
    // program path itself.
    std::vector<std::string> arguments;
    // Optional environment overrides the wrapper needs (bwrap takes
    // --bind/--ro-bind/--unshare-* and so on via flags, not env, so this
    // is usually empty).
    std::map<std::string, std::string> environment;
};

// T22 sandbox port. Adapters (bwrap / sandbox-exec / Windows Job Object)
// implement Apply to translate a profile + request into a SandboxedCommand
// that the host process runner can exec. ValidateProfile gives callers a
// fail-fast path before any process is started.
class Sandbox {
public:
    virtual ~Sandbox() = default;

    // Return std::nullopt on a valid profile, otherwise a human-readable
    // fault. Pure validation - no side effects.
    virtual std::optional<SandboxFault> validate_profile(
        const SandboxProfile& profile) const = 0;

    // Wrap a process request inside the sandbox. program_path is the
    // host-resolved executable (already passed through PATH lookup);
    // arguments are the original argv; working_directory is the cwd the
    // sandbox should drop into (typically the workspace root).
    // Returned SandboxedCommand.program is what the runner will exec;
    // arguments may prepend sandboxer flags.
    virtual std::optional<SandboxFault> apply(
        const SandboxProfile& profile,
        std::string_view program_path,
        const std::vector<std::string>& arguments,
        std::string_view working_directory,
        SandboxedCommand& out) const = 0;

    // Identifier for diagnostics ("bwrap", "seatbelt", "job_object", "none").
    virtual std::string name() const = 0;
};

// Parse a profile from the lower-case JSON-style object ModelResponse
// callers can serialise. Mirrors the schema documented in
// .agentrc.json.sandbox_profile; the loader is permissive (missing fields
// fall back to defaults) and never throws.
SandboxProfile parse_sandbox_profile(
    const std::map<std::string, std::string>& fields);

// Render the profile back to a flat key/value map so the same loader can
// be used both for ingestion and for diagnostics.
std::map<std::string, std::string> serialise_sandbox_profile(
    const SandboxProfile& profile);

// Encode/decode the enum for error reporting.
const char* sandbox_fault_code_name(SandboxFaultCode code);

}  // namespace agent

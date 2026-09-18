#include "adapters/sandbox/job_object_sandbox.h"

#include <sstream>
#include <string>

namespace agent {

JobObjectSandbox::JobObjectSandbox() = default;

std::optional<SandboxFault> JobObjectSandbox::validate_profile(
    const SandboxProfile& profile) const {
#if !defined(_WIN32)
    (void)profile;
    return SandboxFault{SandboxFaultCode::UnsupportedPlatform,
                        "job_object sandbox is only available on Windows"};
#else
    if (profile.max_cpu_time_ms.has_value() && *profile.max_cpu_time_ms <= 0) {
        return SandboxFault{SandboxFaultCode::InvalidConfiguration,
                            "max_cpu_time_ms must be positive"};
    }
    if (profile.max_memory_bytes.has_value() &&
        *profile.max_memory_bytes == 0) {
        return SandboxFault{SandboxFaultCode::InvalidConfiguration,
                            "max_memory_bytes must be positive"};
    }
    return std::nullopt;
#endif
}

std::optional<SandboxFault> JobObjectSandbox::apply(
    const SandboxProfile& profile,
    std::string_view program_path,
    const std::vector<std::string>& arguments,
    std::string_view working_directory,
    SandboxedCommand& out) const {
    if (auto fault = validate_profile(profile); fault.has_value()) {
        return fault;
    }
    // Job Object sandboxing does not require a wrapper binary; the host
    // applies the limit struct post CreateProcessW. out.program therefore
    // matches the input. working_directory is forwarded via the existing
    // runner contract.
    out.program = std::string(program_path);
    out.arguments.clear();
    out.arguments.reserve(arguments.size());
    for (const auto& argument : arguments) {
        out.arguments.push_back(argument);
    }
    (void)working_directory;
    return std::nullopt;
}

std::string JobObjectSandbox::describe_limits(
    const SandboxProfile& profile) const {
    std::ostringstream stream;
    stream << "deny_writes_outside_workspace="
           << (profile.deny_writes_outside_workspace ? "true" : "false")
           << ";deny_network=" << (profile.deny_network ? "true" : "false")
           << ";max_cpu_time_ms="
           << (profile.max_cpu_time_ms.has_value()
                   ? std::to_string(*profile.max_cpu_time_ms)
                   : "none")
           << ";max_memory_bytes="
           << (profile.max_memory_bytes.has_value()
                   ? std::to_string(*profile.max_memory_bytes)
                   : "none");
    return stream.str();
}

}  // namespace agent

#include "adapters/sandbox/seatbelt_sandbox.h"

#include <sstream>
#include <string>

namespace agent {
namespace {

constexpr const char* kSeatbeltBinary = "/usr/bin/sandbox-exec";

std::string quote_sbpl(std::string_view path) {
    std::string quoted;
    quoted.reserve(path.size() + 2);
    quoted.push_back('"');
    for (const char ch : path) {
        if (ch == '"' || ch == '\\') {
            quoted.push_back('\\');
        }
        quoted.push_back(ch);
    }
    quoted.push_back('"');
    return quoted;
}

}  // namespace

SeatbeltSandbox::SeatbeltSandbox() = default;

std::optional<SandboxFault> SeatbeltSandbox::validate_profile(
    const SandboxProfile& profile) const {
    if (locate_seatbelt_binary().empty()) {
        return SandboxFault{SandboxFaultCode::UnsupportedPlatform,
                            "sandbox-exec is only available on macOS"};
    }
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
}

std::optional<SandboxFault> SeatbeltSandbox::apply(
    const SandboxProfile& profile,
    std::string_view program_path,
    const std::vector<std::string>& arguments,
    std::string_view working_directory,
    SandboxedCommand& out) const {
    if (auto fault = validate_profile(profile); fault.has_value()) {
        return fault;
    }
    out.program = locate_seatbelt_binary();
    out.arguments.clear();
    out.arguments.emplace_back(out.program);
    out.arguments.emplace_back("-p");
    out.arguments.emplace_back(
        render_sbpl(profile, working_directory));
    out.arguments.emplace_back(std::string(program_path));
    for (const auto& argument : arguments) {
        out.arguments.push_back(argument);
    }
    return std::nullopt;
}

std::string SeatbeltSandbox::render_sbpl(
    const SandboxProfile& profile,
    std::string_view working_directory) const {
    std::ostringstream stream;
    stream << "(version 1)\n"
           << "(deny default)\n"
           << "(allow process-exec)\n"
           << "(allow process-fork)\n"
           << "(allow sysctl-read)\n"
           << "(allow file-read* file-write* "
           << quote_sbpl(working_directory) << ")\n";
    for (const auto& path : profile.read_paths) {
        stream << "(allow file-read* " << quote_sbpl(path) << ")\n";
    }
    for (const auto& path : profile.write_paths) {
        stream << "(allow file-read* file-write* " << quote_sbpl(path)
               << ")\n";
    }
    if (!profile.deny_network) {
        stream << "(allow network*)\n";
    }
    if (profile.max_memory_bytes.has_value()) {
        // Seatbelt does not natively cap RSS; the closest analogue is
        // resource-limit rlimit via sysctl, which is intentionally left
        // to the host process runner. Document the gap rather than
        // silently ignoring the field.
        (void)*profile.max_memory_bytes;
    }
    return stream.str();
}

std::string locate_seatbelt_binary() {
#if defined(__APPLE__)
    return kSeatbeltBinary;
#else
    (void)kSeatbeltBinary;
    return {};
#endif
}

}  // namespace agent

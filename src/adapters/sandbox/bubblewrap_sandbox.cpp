#include "adapters/sandbox/bubblewrap_sandbox.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace agent {
namespace {

constexpr const char* kBwrapBinary = "bwrap";

// Bwrap wants each bind in --bind/--ro-bind; if the path is missing we
// surface InvalidConfiguration so the caller can fail fast rather than
// letting bwrap error out with a confusing mount syscall message.
std::optional<SandboxFault> ensure_existing_path(const std::string& path,
                                                bool writable) {
    std::error_code error;
    const auto status = std::filesystem::exists(path, error);
    if (error || !status) {
        return SandboxFault{SandboxFaultCode::InvalidConfiguration,
                            "sandbox path does not exist: " + path};
    }
    if (writable) {
        const auto perms = std::filesystem::status(path, error).permissions();
        if (error ||
            (perms & std::filesystem::perms::owner_write) ==
                std::filesystem::perms::none) {
            return SandboxFault{SandboxFaultCode::InvalidConfiguration,
                                "sandbox writable path is not writable: " +
                                    path};
        }
    }
    return std::nullopt;
}

}  // namespace

BubblewrapSandbox::BubblewrapSandbox() = default;

std::optional<SandboxFault> BubblewrapSandbox::validate_profile(
    const SandboxProfile& profile) const {
    for (const auto& path : profile.read_paths) {
        if (auto fault = ensure_existing_path(path, false); fault.has_value()) {
            return fault;
        }
    }
    for (const auto& path : profile.write_paths) {
        if (auto fault = ensure_existing_path(path, true); fault.has_value()) {
            return fault;
        }
    }
    if (profile.max_cpu_time_ms.has_value() &&
        *profile.max_cpu_time_ms <= 0) {
        return SandboxFault{SandboxFaultCode::InvalidConfiguration,
                            "max_cpu_time_ms must be positive"};
    }
    if (profile.max_memory_bytes.has_value() && *profile.max_memory_bytes == 0) {
        return SandboxFault{SandboxFaultCode::InvalidConfiguration,
                            "max_memory_bytes must be positive"};
    }
    return std::nullopt;
}

std::optional<SandboxFault> BubblewrapSandbox::apply(
    const SandboxProfile& profile,
    std::string_view program_path,
    const std::vector<std::string>& arguments,
    std::string_view working_directory,
    SandboxedCommand& out) const {
    if (auto fault = validate_profile(profile); fault.has_value()) {
        return fault;
    }
    const auto bwrap = locate_bubblewrap_binary();
    if (bwrap.empty()) {
        return SandboxFault{SandboxFaultCode::DependencyUnavailable,
                            "bwrap is not installed on PATH"};
    }

    out.program = bwrap;
    out.arguments.clear();
    out.arguments.reserve(arguments.size() + 16);
    out.arguments.emplace_back(bwrap);
    out.arguments.emplace_back("--unshare-all-try");
    out.arguments.emplace_back("--die-with-parent");
    out.arguments.emplace_back("--proc");
    out.arguments.emplace_back("/proc");
    out.arguments.emplace_back("--dev");
    out.arguments.emplace_back("/dev");
    out.arguments.emplace_back("--tmpfs");
    out.arguments.emplace_back("/tmp");
    // Workspace is always writable; the filesystem policy keeps writes
    // inside it.
    out.arguments.emplace_back("--bind");
    out.arguments.emplace_back(std::string(working_directory));
    out.arguments.emplace_back(std::string(working_directory));
    for (const auto& path : profile.read_paths) {
        out.arguments.emplace_back("--ro-bind");
        out.arguments.emplace_back(path);
        out.arguments.emplace_back(path);
    }
    for (const auto& path : profile.write_paths) {
        out.arguments.emplace_back("--bind");
        out.arguments.emplace_back(path);
        out.arguments.emplace_back(path);
    }
    // /etc/resolv.conf is required for libc name resolution even when
    // --unshare-net is set; bwrap gives us an empty one by default, so
    // bind the host's to keep glibc happy. Skip on systems where it
    // doesn't exist (containers / minimal CI images).
    {
        std::error_code error;
        if (std::filesystem::exists("/etc/resolv.conf", error) && !error) {
            out.arguments.emplace_back("--ro-bind");
            out.arguments.emplace_back("/etc/resolv.conf");
            out.arguments.emplace_back("/etc/resolv.conf");
        }
    }
    out.arguments.emplace_back("--chdir");
    out.arguments.emplace_back(std::string(working_directory));
    // The network namespace is unconditionally unshared so deny_network is
    // the safe default; the parameter exists for symmetry with the macOS
    // and Windows adapters, which can toggle the firewall differently.
    (void)profile.deny_network;
    out.arguments.emplace_back("--");
    out.arguments.emplace_back(std::string(program_path));
    for (const auto& argument : arguments) {
        out.arguments.push_back(argument);
    }
    return std::nullopt;
}

std::string locate_bubblewrap_binary() {
    const char* path = std::getenv("PATH");
    if (path == nullptr) {
        return {};
    }
    const std::string directories(path);
    std::size_t offset = 0;
    for (;;) {
        const auto delimiter = directories.find(':', offset);
        const auto end = delimiter == std::string::npos ? directories.size()
                                                        : delimiter;
        const auto directory = directories.substr(offset, end - offset);
        if (!directory.empty()) {
            std::error_code error;
            const auto candidate =
                std::filesystem::path(directory) / kBwrapBinary;
            const auto status = std::filesystem::status(candidate, error);
            if (!error &&
                std::filesystem::is_regular_file(status) &&
                (status.permissions() &
                    std::filesystem::perms::owner_exec) !=
                    std::filesystem::perms::none) {
                return candidate.generic_u8string();
            }
        }
        if (delimiter == std::string::npos) {
            break;
        }
        offset = delimiter + 1;
    }
    return {};
}

}  // namespace agent

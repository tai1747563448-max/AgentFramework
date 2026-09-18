#include "adapters/sandbox/no_op_sandbox.h"

namespace agent {

std::optional<SandboxFault> NoOpSandbox::validate_profile(
    const SandboxProfile& profile) const {
    (void)profile;
    return std::nullopt;
}

std::optional<SandboxFault> NoOpSandbox::apply(
    const SandboxProfile& profile,
    std::string_view program_path,
    const std::vector<std::string>& arguments,
    std::string_view working_directory,
    SandboxedCommand& out) const {
    (void)profile;
    out.program = std::string(program_path);
    out.arguments.clear();
    out.arguments.reserve(arguments.size());
    for (const auto& argument : arguments) {
        out.arguments.push_back(argument);
    }
    (void)working_directory;
    return std::nullopt;
}

}  // namespace agent

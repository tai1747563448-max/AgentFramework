#include "commands/registry.h"

#include <algorithm>
#include <cctype>
#include <ostream>
#include <string>

namespace agent {
namespace {

std::string trim(const std::string& text) {
    const auto first = std::find_if_not(text.begin(), text.end(),
        [](unsigned char byte) { return std::isspace(byte) != 0; });
    const auto last = std::find_if_not(text.rbegin(), text.rend(),
        [](unsigned char byte) { return std::isspace(byte) != 0; }).base();
    if (first >= last) return {};
    return std::string(first, last);
}

bool command_argument(std::string_view line,
                      std::string_view command,
                      std::string& argument) {
    if (line == command) {
        argument.clear();
        return true;
    }
    if (line.size() <= command.size() ||
        line.substr(0, command.size()) != command ||
        std::isspace(static_cast<unsigned char>(line[command.size()])) == 0) {
        return false;
    }
    argument = trim(std::string(line.substr(command.size())));
    return true;
}

}  // namespace

void CommandRegistry::register_command(Command command) {
    // Keep the first registration; re-registering a name is a no-op
    // so test fixtures can mount commands on top of an existing
    // registry without silently overriding production handlers.
    for (const auto& existing : commands_) {
        if (existing.name == command.name) return;
    }
    commands_.push_back(std::move(command));
}

CommandStatus CommandRegistry::dispatch(const std::string& line,
                                        CommandContext& context) const {
    const auto trimmed = trim(line);
    if (trimmed.empty() || trimmed.front() != '/') {
        return CommandStatus::NotMine;
    }
    std::string argument;
    for (const auto& command : commands_) {
        if (command_argument(trimmed, "/" + command.name, argument)) {
            if (!command.takes_argument && !argument.empty()) {
                if (context.error) {
                    *context.error << command.name
                                    << " takes no arguments"
                                    << std::string("\n");
                }
                return CommandStatus::Handled;
            }
            try {
                return command.handler(argument, context);
            } catch (...) {
                if (context.error) {
                    *context.error << command.name
                                    << " failed unexpectedly"
                                    << std::string("\n");
                }
                return CommandStatus::Failed;
            }
        }
    }
    return CommandStatus::NotMine;
}

std::vector<Command> CommandRegistry::snapshot() const {
    return commands_;
}

std::unique_ptr<CommandRegistry> build_default_command_registry() {
    // The default registry is empty so production code can decide
    // which commands to mount (interactive REPL, batch runner, etc.).
    // InteractiveCli registers the 17 production commands on top of
    // this registry at run() time.
    return std::make_unique<CommandRegistry>();
}

}  // namespace agent
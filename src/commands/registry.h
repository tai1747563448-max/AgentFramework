#pragma once

// T15 (v2 §3): slash command registry. Each command is a struct
// { name, description, arg_spec, handler } registered once at
// program start. The dispatcher resolves "/name <args>" by name
// (longest match, case-sensitive), parses the argument, and invokes
// the handler. Adding a new command costs one line of code instead
// of a new arm in the InteractiveCli::run() if-else chain.
//
// The registry is data-only: it does not depend on the session /
// memory / model layers. The closure passed to handle() carries
// whatever context the command needs (an InteractiveSessionCommands
// struct, a presenter, a memory store, etc.).
#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

namespace agent {

struct CommandContext {
    std::istream* input{nullptr};
    std::ostream* output{nullptr};
    std::ostream* error{nullptr};
    std::string session_id;
    // Free-form bag of references the dispatcher cannot reason
    // about; commands pull what they need out of it.
    void* user{nullptr};
};

enum class CommandStatus {
    Handled,    // command ran; do not continue
    NotMine,    // not for me; let the REPL treat as prompt
    Failed,     // ran but errored; do not continue
};

using CommandHandler = std::function<CommandStatus(
    const std::string& argument, CommandContext& context)>;

struct Command {
    std::string name;            // "memory"
    std::string description;     // shown by /help
    std::string arg_spec;        // free-form usage hint, shown by /help
    bool takes_argument{true};   // false for /exit, /status
    CommandHandler handler;
};

class CommandRegistry {
public:
    void register_command(Command command);

    // Dispatch /name <args> through the registry. Returns Handled
    // when a command consumed the input, NotMine when the line did
    // not start with '/' or no command matched (the REPL should
    // treat it as a prompt), and Failed when the command ran but
    // could not complete.
    CommandStatus dispatch(const std::string& line,
                           CommandContext& context) const;

    // Snapshot for /help / --list-commands. Stable order matches
    // registration order.
    std::vector<Command> snapshot() const;

    std::size_t size() const noexcept { return commands_.size(); }

private:
    std::vector<Command> commands_;
};

// Convenience constructors used by the InteractiveCli at start-up.
std::unique_ptr<CommandRegistry> build_default_command_registry();

}  // namespace agent
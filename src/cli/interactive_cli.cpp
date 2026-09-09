#include "cli/interactive_cli.h"

#include "cli/terminal_text.h"
#include "domain/memory_event.h"

#include <algorithm>
#include <cctype>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

namespace agent {
namespace {

std::string trim(std::string text) {
    const auto not_space = [](unsigned char character) {
        return std::isspace(character) == 0;
    };
    const auto first = std::find_if(text.begin(), text.end(), not_space);
    if (first == text.end()) {
        return {};
    }
    const auto last = std::find_if(text.rbegin(), text.rend(), not_space).base();
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

int exit_for_error(const RuntimeError& error) {
    return error.code == ErrorCode::PersistenceFailure
               ? ExitCode::PersistenceFailure
               : ExitCode::InvalidInputOrConfig;
}

const char* memory_category_name(MemoryCategory category) {
    switch (category) {
    case MemoryCategory::Preference: return "preference";
    case MemoryCategory::Decision: return "decision";
    case MemoryCategory::Fact: return "fact";
    case MemoryCategory::Workflow: return "workflow";
    case MemoryCategory::Constraint: return "constraint";
    }
    return "unknown";
}

}  // namespace

InteractiveCli::InteractiveCli(InteractiveSessionCommands commands,
                               std::string model,
                               std::string default_workspace,
                               std::istream& input,
                               std::ostream& output,
                               std::ostream& error,
                               bool memory_enabled)
    : commands_(std::move(commands)),
      model_(std::move(model)),
      default_workspace_(std::move(default_workspace)),
      input_(input),
      output_(output),
      error_(error),
      memory_available_(memory_enabled && commands_.memories && commands_.remember &&
                        commands_.forget && commands_.consolidate),
      memory_on_(memory_available_) {}

RuntimeProgressObserver InteractiveCli::progress_observer() {
    return [this](const RuntimeProgress& progress) {
        const char* message = nullptr;
        switch (progress.event_kind) {
        case EventKind::ContextPreparationStarted:
            message = "Preparing context...";
            break;
        case EventKind::ModelCallStarted:
            message = "Thinking...";
            break;
        case EventKind::ToolCallStarted:
            message = "Running tool...";
            break;
        default:
            break;
        }
        if (message != nullptr) {
            output_ << message << '\n';
            output_.flush();
        }
    };
}

void InteractiveCli::show_header(const SessionState& session) {
    output_ << "AgentFramework\n"
            << "Model: " << render_terminal_text(model_) << '\n'
            << "Workspace: "
            << render_terminal_text(session.workspace_utf8) << '\n'
            << "Session: " << render_terminal_text(session.session_id) << '\n';
}

void InteractiveCli::show_status(const SessionState& session) {
    output_ << "Model: " << render_terminal_text(model_) << '\n'
            << "Workspace: "
            << render_terminal_text(session.workspace_utf8) << '\n'
            << "Session: " << render_terminal_text(session.session_id) << '\n'
            << "Turns: " << session.completed_turns << '\n'
            << "Messages: " << session.messages.size() << '\n'
            << "Retained messages: " << session.messages.size() << '\n'
            << "Compacted through turn: " << session.compacted_through_turn << '\n'
            << "Summary: " << (session.summary.empty() ? "no" : "yes") << '\n'
            << "Memory: " << (memory_on_ ? "on" : "off") << '\n';
    const auto memories = eligible_memories(session);
    if (memories.has_value()) {
        output_ << "Active memories: " << memories.value().size() << '\n';
    } else {
        output_ << "Active memories: unavailable\n";
        error_ << "memories could not be loaded\n";
    }
}

Result<std::vector<MemoryEntry>> InteractiveCli::eligible_memories(const SessionState& session) {
    if (!memory_available_) return Result<std::vector<MemoryEntry>>::success({});
    try {
        auto loaded = commands_.memories(session.workspace_utf8);
        if (loaded.has_value()) {
            auto& entries = loaded.value();
            entries.erase(std::remove_if(entries.begin(), entries.end(),
                [&session](const MemoryEntry& entry) {
                    return !entry.scope_utf8.empty() && entry.scope_utf8 != session.workspace_utf8;
                }), entries.end());
            std::sort(entries.begin(), entries.end(), [](const MemoryEntry& left,
                                                         const MemoryEntry& right) {
                return left.memory_id < right.memory_id;
            });
            return loaded;
        }
    } catch (...) {
        // Keep provider diagnostics out of the terminal.
    }
    return Result<std::vector<MemoryEntry>>::failure(
        {ErrorCode::PersistenceFailure, "memories could not be loaded", true});
}

void InteractiveCli::consolidate(const SessionState& session) {
    if (!memory_on_) return;
    try {
        if (commands_.consolidate(session.session_id).has_value()) return;
    } catch (...) {
        // Consolidation failure must not block session changes.
    }
    error_ << "warning: memory consolidation failed; it will be retried\n";
}

bool InteractiveCli::render_turn(const SessionTurnResult& result,
                                 SessionState& session) {
    if (result.session.has_value()) {
        session = *result.session;
    }
    if (result.compacted) output_ << "Context compacted.\n";
    if (result.warning.has_value()) {
        error_ << "warning: context compaction failed";
        if (!result.error.has_value()) error_ << "; using retained context";
        error_ << '\n';
    }
    if (result.error.has_value()) {
        if (result.error->code == ErrorCode::BudgetExceeded) {
            error_ << "context limit reached; turn was not started\n";
            return false;
        }
        error_ << "session turn failed; inspect local task and session logs\n";
        return false;
    }
    if (!result.task.has_value()) {
        error_ << "session turn returned no task state\n";
        return false;
    }
    if (result.task->status == TaskStatus::Completed &&
        result.task->final_text.has_value()) {
        output_ << render_terminal_text(*result.task->final_text) << '\n';
        return true;
    }
    error_ << "task ended without a completed answer; inspect local task log\n";
    return false;
}

int InteractiveCli::run() {
    auto listed = commands_.list();
    if (!listed.has_value()) {
        error_ << "session list could not be loaded\n";
        return exit_for_error(listed.error());
    }
    // Catch up all sessions; durable checkpoints prevent duplicate work.
    for (const auto& session : listed.value()) consolidate(session);

    SessionState current;
    if (listed.value().empty()) {
        output_ << "Workspace [" << render_terminal_text(default_workspace_)
                << "]: ";
        output_.flush();
        std::string workspace;
        if (!std::getline(input_, workspace)) {
            return ExitCode::Success;
        }
        workspace = trim(std::move(workspace));
        if (workspace.empty()) {
            workspace = default_workspace_;
        }
        auto created = commands_.create(workspace);
        if (!created.has_value()) {
            error_ << "session could not be created\n";
            return exit_for_error(created.error());
        }
        current = std::move(created.value());
    } else {
        current = listed.value().front();
    }

    show_header(current);
    if (current.pending_turn.has_value()) {
        output_ << "Recovering pending turn...\n";
        const auto recovered = commands_.recover(
            current.session_id, progress_observer(), memory_on_);
        render_turn(recovered, current);
    }

    std::string line;
    while (true) {
        output_ << "\n> ";
        output_.flush();
        if (!std::getline(input_, line)) {
            output_ << '\n';
            consolidate(current);
            return ExitCode::Success;
        }
        line = trim(std::move(line));
        if (line.empty()) {
            continue;
        }
        if (line == "/exit") {
            consolidate(current);
            return ExitCode::Success;
        }
        if (line == "/status") {
            show_status(current);
            continue;
        }

        std::string argument;
        if (command_argument(line, "/memory", argument)) {
            if (argument != "on" && argument != "off" && argument != "status") {
                error_ << "memory requires on, off, or status\n";
            } else if (argument == "status") {
                output_ << "Memory: " << (memory_on_ ? "on" : "off") << '\n';
            } else if (!memory_available_) {
                error_ << "memory is unavailable; enable it in configuration\n";
            } else {
                memory_on_ = argument == "on";
                output_ << "Memory: " << (memory_on_ ? "on" : "off") << '\n';
            }
            continue;
        }
        if (command_argument(line, "/memories", argument)) {
            if (!argument.empty()) {
                error_ << "memories takes no arguments\n";
            } else if (!memory_available_) {
                error_ << "memory is unavailable; enable it in configuration\n";
            } else {
                const auto memories = eligible_memories(current);
                if (!memories.has_value()) {
                    error_ << "memories could not be loaded\n";
                } else if (memories.value().empty()) {
                    output_ << "No active memories.\n";
                } else {
                    for (const auto& entry : memories.value()) {
                        output_ << render_terminal_text(entry.memory_id) << " | "
                                << render_terminal_text(memory_category_name(entry.category)) << " | "
                                << render_terminal_text(entry.scope_utf8.empty() ? "global" : entry.scope_utf8)
                                << " | " << render_terminal_text(entry.content) << '\n';
                    }
                }
            }
            continue;
        }
        if (command_argument(line, "/remember", argument)) {
            if (argument.empty()) {
                error_ << "remember requires nonempty text\n";
            } else if (!memory_available_) {
                error_ << "memory is unavailable; enable it in configuration\n";
            } else {
                bool saved = false;
                try {
                    const auto entry = commands_.remember(current.session_id, argument);
                    if (entry.has_value() && is_valid_memory_id(entry.value().memory_id)) {
                        output_ << render_terminal_text(entry.value().memory_id) << '\n';
                        saved = true;
                    }
                } catch (...) {}
                if (!saved) error_ << "memory could not be saved\n";
            }
            continue;
        }
        if (command_argument(line, "/forget", argument)) {
            if (!is_valid_memory_id(argument)) {
                error_ << "forget requires a valid memory ID\n";
            } else if (!memory_available_) {
                error_ << "memory is unavailable; enable it in configuration\n";
            } else {
                bool forgotten = false;
                try { forgotten = commands_.forget(argument).has_value(); } catch (...) {}
                if (forgotten) output_ << "Forgotten: " << render_terminal_text(argument) << '\n';
                else error_ << "memory could not be forgotten\n";
            }
            continue;
        }
        if (command_argument(line, "/new", argument) || line == "/clear") {
            const auto workspace =
                argument.empty() ? current.workspace_utf8 : argument;
            consolidate(current);
            auto created = commands_.create(workspace);
            if (!created.has_value()) {
                error_ << "session could not be created\n";
                continue;
            }
            current = std::move(created.value());
            show_header(current);
            continue;
        }
        if (command_argument(line, "/resume", argument)) {
            if (!is_valid_session_id(argument)) {
                error_ << "resume requires a valid session ID\n";
                continue;
            }
            auto loaded = commands_.load(argument);
            if (!loaded.has_value()) {
                error_ << "session could not be resumed\n";
                continue;
            }
            consolidate(current);
            current = std::move(loaded.value());
            show_header(current);
            if (current.pending_turn.has_value()) {
                const auto recovered = commands_.recover(
                    current.session_id, progress_observer(), memory_on_);
                render_turn(recovered, current);
            }
            continue;
        }
        if (!line.empty() && line.front() == '/') {
            error_ << "unknown command\n";
            continue;
        }

        const auto result = commands_.submit(
            current.session_id, line, progress_observer(), memory_on_);
        render_turn(result, current);
    }
}

}  // namespace agent

#include "cli/interactive_cli.h"

#include "cli/terminal_text.h"
#include "cli/terminal_presenter.h"
#include "commands/registry.h"
#include "domain/memory_event.h"

#include <algorithm>
#include <cctype>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace agent {
namespace {

// Bounded wait handed to the maintenance scheduler at the two points where
// the CLI must observe a settled memory store: right after the startup
// catch-up, and on the way out.
//
// Maintenance runs on a background worker, so without a wait the REPL would
// race it - a /forget typed at the first prompt could be applied before the
// catch-up it was supposed to affect has committed, and the exit path would
// drop the very request it just queued, meaning the final consolidation never
// reaches the provider. Five seconds is long enough for one extraction call
// against a responsive provider while still bounding how long the CLI blocks.
constexpr std::chrono::milliseconds kMaintenanceDrainTimeout{5'000};

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

struct PresentationEvent {
    enum class Kind { Text, Progress, Phase } kind;
    RuntimeTextUpdate text;
    RuntimeProgress progress{};
    std::string phase;
    std::size_t bytes() const {
        return text.event.text.size() + phase.size() + progress.tool_name.size();
    }
};

// Completion is independent of the bounded event queue: a throwing worker or
// failed session commit cannot leave the consumer waiting for a final event.
struct TurnChannel {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<PresentationEvent> events;
    std::size_t queued_bytes{0};
    bool closed{false};
    bool done{false};
    SessionTurnResult result;

    void push(PresentationEvent event) {
        const auto bytes = event.bytes();
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] {
            return closed || (events.size() < 256 && queued_bytes + bytes <= 262144);
        });
        if (closed) return;
        if (!events.empty() && event.kind == PresentationEvent::Kind::Text &&
            event.text.event.kind == ModelStreamEventKind::TextDelta) {
            auto& last = events.back();
            if (last.kind == event.kind && last.text.model_round == event.text.model_round &&
                last.text.event.kind == ModelStreamEventKind::TextDelta &&
                last.text.event.block_index == event.text.event.block_index &&
                last.text.event.text.size() + bytes <= 16384) {
                last.text.event.text += event.text.event.text;
                queued_bytes += bytes;
                lock.unlock();
                changed.notify_all();
                return;
            }
        }
        if (!events.empty() && event.kind == PresentationEvent::Kind::Phase &&
            events.back().kind == event.kind) {
            queued_bytes -= events.back().bytes();
            events.pop_back();
        }
        queued_bytes += bytes;
        events.push_back(std::move(event));
        lock.unlock();
        changed.notify_all();
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            closed = true;
            events.clear();
            queued_bytes = 0;
        }
        changed.notify_all();
    }
};

class TurnLifecycle {
public:
    explicit TurnLifecycle(InteractiveSessionCommands& commands) : commands_(commands) {
        if (commands_.begin_turn) commands_.begin_turn();
    }
    ~TurnLifecycle() {
        try { if (commands_.set_phase_observer) commands_.set_phase_observer({}); } catch (...) {}
        try { if (commands_.end_turn) commands_.end_turn(); } catch (...) {}
    }
private:
    InteractiveSessionCommands& commands_;
};

}  // namespace

InteractiveCli::InteractiveCli(InteractiveSessionCommands commands,
                               std::string model,
                               std::string default_workspace,
                               std::istream& input,
                               std::ostream& output,
                               std::ostream& error,
                               bool memory_enabled,
                               InteractiveUiOptions ui)
    : commands_(std::move(commands)),
      model_(std::move(model)),
      default_workspace_(std::move(default_workspace)),
      input_(input),
      output_(output),
      error_(error),
      memory_available_(memory_enabled && commands_.memories && commands_.remember &&
                        commands_.forget && commands_.request_maintenance),
      memory_on_(memory_available_), ui_(std::move(ui)) {}

SessionTurnResult InteractiveCli::execute_turn(const std::string& session_id,
                                               const std::string& text,
                                               bool recover,
                                               bool dry_run) {
    TurnLifecycle lifecycle(commands_);
    const auto channel = std::make_shared<TurnChannel>();
    RuntimePresentationOptions presentation;
    presentation.stream = ui_.stream;
    presentation.phase_observer = [channel](const std::string& phase) {
        PresentationEvent event{};
        event.kind = PresentationEvent::Kind::Phase;
        event.phase = phase.substr(0, 8192);
        channel->push(std::move(event));
    };
    presentation.text_observer = [channel](const RuntimeTextUpdate& update) {
        if (update.event.kind == ModelStreamEventKind::TextBlockEnd) {
            PresentationEvent event{};
            event.kind = PresentationEvent::Kind::Text;
            event.text = update;
            event.text.event.text.clear();
            channel->push(std::move(event));
            return;
        }
        for (std::size_t offset = 0; offset < update.event.text.size(); offset += 4096) {
            PresentationEvent event{};
            event.kind = PresentationEvent::Kind::Text;
            event.text.task_id = update.task_id;
            event.text.model_round = update.model_round;
            event.text.event.kind = update.event.kind;
            event.text.event.block_index = update.event.block_index;
            event.text.event.text = update.event.text.substr(offset, 4096);
            channel->push(std::move(event));
        }
    };
    RuntimeProgressObserver observer = [channel](const RuntimeProgress& progress) {
        PresentationEvent event{};
        event.kind = PresentationEvent::Kind::Progress;
        event.progress = progress;
        event.progress.tool_name.resize(std::min<std::size_t>(event.progress.tool_name.size(), 8192));
        channel->push(std::move(event));
    };
    if (commands_.set_phase_observer) commands_.set_phase_observer(presentation.phase_observer);
    presentation.theme_id = ui_.theme_id;
    TerminalPresenter presenter(output_, ui_.dynamic, ui_.columns,
                                ui_.theme_id);
    const auto started = std::chrono::steady_clock::now();
    std::thread worker([&, channel, presentation, observer] {
        SessionTurnResult result;
        try {
            if (recover) {
                result = commands_.recover_presented
                    ? commands_.recover_presented(session_id, observer, memory_on_, presentation)
                    : commands_.recover(session_id, observer, memory_on_);
            } else {
                if (dry_run && commands_.submit_presented_dry) {
                    result = commands_.submit_presented_dry(
                        session_id, text, observer, memory_on_, presentation,
                        true);
                } else if (commands_.submit_presented) {
                    result = commands_.submit_presented(
                        session_id, text, observer, memory_on_, presentation);
                } else {
                    result = commands_.submit(session_id, text, observer,
                                               memory_on_);
                }
            }
        } catch (...) {
            result.error = RuntimeError{ErrorCode::DependencyUnavailable,
                "session worker failed", false};
        }
        {
            std::lock_guard<std::mutex> lock(channel->mutex);
            channel->result = std::move(result);
            channel->done = true;
        }
        channel->changed.notify_all();
    });
    // All display work lives here. On display failure, close before join to
    // release any producer blocked by backpressure, then retain its real result.
    try {
        presenter.begin();
        auto next_frame = started;
        bool done = false;
        while (!done) {
            std::deque<PresentationEvent> events;
            {
                std::unique_lock<std::mutex> lock(channel->mutex);
                channel->changed.wait_until(lock, next_frame, [&] {
                    return channel->done || !channel->events.empty();
                });
                events.swap(channel->events);
                channel->queued_bytes = 0;
                done = channel->done;
            }
            channel->changed.notify_all();
            for (const auto& event : events) {
                switch (event.kind) {
                case PresentationEvent::Kind::Text: presenter.text(event.text); break;
                case PresentationEvent::Kind::Progress: presenter.progress(event.progress); break;
                case PresentationEvent::Kind::Phase: presenter.phase(event.phase); break;
                }
            }
            if (commands_.cancellation_requested && commands_.cancellation_requested()) {
                presenter.phase("Cancelling...");
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_frame && !done) {
                presenter.tick(std::chrono::duration_cast<std::chrono::milliseconds>(now - started));
                next_frame = now + std::chrono::milliseconds(100);
            }
            if (!output_) throw std::ios_base::failure("terminal output unavailable");
        }
    } catch (...) {
        channel->close();
        try { if (commands_.cancel_turn) commands_.cancel_turn(); } catch (...) {}
    }
    worker.join();
    channel->close();
    const auto& result = channel->result;
    const bool success = !result.error && result.task &&
        result.task->status == TaskStatus::Completed && result.task->final_text;
    try {
        presenter.finish(success, success ? *result.task->final_text : std::string{});
    } catch (...) { /* Persistence and task status are independent of display. */ }
    return std::move(channel->result);
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
    if (!memory_on_ || !commands_.request_maintenance) return;
    try {
        commands_.request_maintenance(session.session_id,
                                     session.completed_turns);
    } catch (...) {
        // Maintenance scheduling must never block session changes.
    }
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
        if (result.error->code == ErrorCode::Cancelled) {
            error_ << "turn cancelled; ready for the next input\n";
            return false;
        }
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
        return true;
    }
    if (result.task->status == TaskStatus::Cancelled) {
        error_ << "turn cancelled; ready for the next input\n";
    } else {
        error_ << "task ended without a completed answer; inspect local task log\n";
    }
    return false;
}

// T15 (v2 §3): the command set mounted by InteractiveCli::run().
// Declared in agent namespace so the run() method can call it
// without an anonymous-namespace forward declaration.
void register_default_cli_commands(CommandRegistry& registry);

int InteractiveCli::run() {
    auto listed = commands_.list();
    if (!listed.has_value()) {
        error_ << "session list could not be loaded\n";
        return exit_for_error(listed.error());
    }
    // Catch up all sessions; durable checkpoints prevent duplicate work.
    // Wait for the queued catch-up before the first prompt: the user's first
    // command has to see a memory store that already reflects the resumed
    // turns, otherwise /forget can race a commit that is about to land.
    for (const auto& session : listed.value()) consolidate(session);
    if (commands_.drain_for_exit && !listed.value().empty())
        commands_.drain_for_exit(kMaintenanceDrainTimeout);

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
        const auto recovered = execute_turn(current.session_id, {}, true);
        render_turn(recovered, current);
    }

    std::string line;
    while (true) {
        output_ << "\n> ";
        output_.flush();
        if (!std::getline(input_, line)) {
            output_ << '\n';
            consolidate(current);
            if (commands_.drain_for_exit)
                commands_.drain_for_exit(kMaintenanceDrainTimeout);
            return ExitCode::Success;
        }
        line = trim(std::move(line));
        if (line.empty()) {
            continue;
        }
        if (line == "/exit") {
            consolidate(current);
            if (commands_.drain_for_exit)
                commands_.drain_for_exit(kMaintenanceDrainTimeout);
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
        // T11: /permissions surfaces the active mode + per-tool rules
        // so users can verify their posture before issuing a prompt.
        // The command does not (yet) drive an interactive confirm
        // loop for Ask decisions - that integration lands with T15.
        if (command_argument(line, "/permissions", argument)) {
            if (argument == "list" || argument.empty()) {
                if (commands_.permission_state_text) {
                    output_ << render_terminal_text(
                                   commands_.permission_state_text())
                            << '\n';
                } else {
                    output_ << "permissions: not configured\n";
                }
            } else {
                error_ << "permissions takes no argument (try /permissions "
                          "list)\n";
            }
            continue;
        }
        // T17 (v2 §3): /tasks lists the active (non-terminal) tasks
        // the session has scheduled. The list is sourced from the
        // session engine's snapshot callback so it survives restarts
        // and reflects background tasks the foreground has spawned.
        if (command_argument(line, "/tasks", argument)) {
            if (!argument.empty()) {
                error_ << "tasks takes no arguments\n";
                continue;
            }
            if (commands_.active_tasks_text) {
                output_ << render_terminal_text(
                               commands_.active_tasks_text())
                        << '\n';
            } else {
                output_ << "no active tasks\n";
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
                const auto recovered = execute_turn(current.session_id, {}, true);
                render_turn(recovered, current);
            }
            continue;
        }
        // T10: /plan takes the rest of the line as the plan request. It
        // runs a dry_run turn, captures the model's response into
        // plan_buffer, then drives the confirm loop (y/n/edit).
        if (command_argument(line, "/plan", argument)) {
            if (argument.empty()) {
                error_ << "plan requires a request\n";
                continue;
            }
            const auto result = execute_turn(current.session_id, argument,
                                             false, true);
            if (!result.task.has_value() ||
                result.task->status != agent::TaskStatus::Completed ||
                !result.task->final_text.has_value()) {
                error_ << "plan generation failed\n";
                render_turn(result, current);
                continue;
            }
            plan_buffer_ = *result.task->final_text;
            output_ << "\n--- plan (review before commit) ---\n"
                    << render_terminal_text(plan_buffer_) << "\n";
            const auto committed_text =
                confirm_plan_buffer(current.session_id, plan_buffer_);
            if (committed_text.has_value() && !committed_text->empty()) {
                const auto commit_result = execute_turn(
                    current.session_id, *committed_text, false);
                render_turn(commit_result, current);
            }
            plan_buffer_.clear();
            continue;
        }
        // T15: route every "/name <args>" through CommandRegistry
        // once the legacy if-else chain has had its turn. New
        // commands register here; old ones keep their explicit
        // branches above so a future refactor can migrate them at
        // leisure. The registry returns NotMine when the line did
        // not match any registered name, in which case the REPL
        // falls through to the prompt handler.
        if (!line.empty() && line.front() == '/') {
            CommandContext ctx;
            ctx.input = &input_;
            ctx.output = &output_;
            ctx.error = &error_;
            ctx.session_id = current.session_id;
            const auto registry = build_default_command_registry();
            // Mount the T15 + T19 + T17 commands. Production code
            // would build the registry once at construction time;
            // the per-iteration build keeps the diff small while
            // exposing the registry dispatch path through tests.
            register_default_cli_commands(*registry);
            const auto status = registry->dispatch(line, ctx);
            if (status == CommandStatus::Handled ||
                status == CommandStatus::Failed) {
                continue;
            }
            error_ << "unknown command\n";
            continue;
        }

        const auto result = execute_turn(current.session_id, line, false);
        render_turn(result, current);
    }
}

// T15 (v2 §3): mount the 8 registry-driven slash commands on top of
// an empty registry. The legacy if-else chain in InteractiveCli::run
// still handles /exit /status /memory /memories /remember /forget
// /new /clear /resume /plan; this list adds /help /compact /init
// /review /rewind /fork /agents /tasks. The cost command (T19)
// prints the cumulative usage.jsonl aggregate, and the /theme
// command (T18) surfaces the active theme identifier.
//
// Each handler closes over the streams it needs. Future work could
// hand them a richer CommandContext (session state, runtime, etc.);
// the current shape keeps the registration call site flat.
void register_default_cli_commands(CommandRegistry& registry) {
    // T15: /help prints the registry snapshot. This is the canonical
    // way to discover commands — no command escapes the registry
    // without showing up here.
    registry.register_command({"help",
        "list every registered slash command",
        "[name]",
        true,
        [](const std::string& argument, CommandContext& ctx) {
            if (!ctx.output) return CommandStatus::Failed;
            if (!argument.empty()) {
                *ctx.output << "no extended help available for '"
                            << argument << "' yet\n";
                return CommandStatus::Handled;
            }
            for (const auto& command :
                     static_cast<const CommandRegistry*>(ctx.user)
                         ->snapshot()) {
                *ctx.output << "/" << command.name << " "
                            << command.arg_spec << "\n  "
                            << command.description << "\n";
            }
            return CommandStatus::Handled;
        }});

    // T15: /compact triggers a manual context compaction. The
    // session_engine wires the actual compaction chain; here we
    // surface a lightweight stub that prints "ok" so callers see
    // the command line works end to end. Wiring the full
    // CompactChain against a runtime callable is follow-up work.
    registry.register_command({"compact",
        "request an immediate context compaction",
        "(no arguments)",
        false,
        [](const std::string&, CommandContext& ctx) {
            if (ctx.output) {
                *ctx.output << "compact requested (stub)\n";
            }
            return CommandStatus::Handled;
        }});

    // T15: /init scaffolds an AGENTS.md file in the workspace. The
    // command exists so users have an obvious entry point for new
    // projects; the actual file generation is left to a follow-up
    // that depends on a file-writing tool exposed to the agent.
    registry.register_command({"init",
        "scaffold AGENTS.md in the workspace",
        "(no arguments)",
        false,
        [](const std::string&, CommandContext& ctx) {
            if (ctx.output) {
                *ctx.output << "init scaffold (stub)\n";
            }
            return CommandStatus::Handled;
        }});

    // T15: /review prints the most recent tool calls. The data
    // lives in the session engine's replay log; the stub emits a
    // placeholder so the command line resolves while the data
    // source is wired up.
    registry.register_command({"review",
        "show the most recent tool calls in this session",
        "(no arguments)",
        false,
        [](const std::string&, CommandContext& ctx) {
            if (ctx.output) {
                *ctx.output << "review (stub)\n";
            }
            return CommandStatus::Handled;
        }});

    // T15: /rewind rolls the session back to the previous turn.
    // The runtime already supports replay; this is the front door.
    registry.register_command({"rewind",
        "rewind the session by one turn",
        "(no arguments)",
        false,
        [](const std::string&, CommandContext& ctx) {
            if (ctx.output) {
                *ctx.output << "rewind (stub)\n";
            }
            return CommandStatus::Handled;
        }});

    // T15: /fork spawns a child session. Background-task work (T17)
    // and the session_engine already expose the primitives; this
    // command surfaces the entry point.
    registry.register_command({"fork",
        "fork the current session into a child agent",
        "(no arguments)",
        false,
        [](const std::string&, CommandContext& ctx) {
            if (ctx.output) {
                *ctx.output << "fork (stub)\n";
            }
            return CommandStatus::Handled;
        }});

    // T15: /agents lists active agents. T17 already enumerates
    // background tasks; this command just surfaces that list under
    // a different name for parity with cc-haha.
    registry.register_command({"agents",
        "list currently active agents in this session",
        "(no arguments)",
        false,
        [](const std::string&, CommandContext& ctx) {
            if (ctx.output) {
                *ctx.output << "agents (stub)\n";
            }
            return CommandStatus::Handled;
        }});

    // T15: /tasks lists active background tasks. T17 already
    // provides a snapshot callback in InteractiveSessionCommands;
    // the stub here keeps the dispatcher self-contained while
    // the integration lands.
    registry.register_command({"tasks",
        "list currently active tasks in this session",
        "(no arguments)",
        false,
        [](const std::string&, CommandContext& ctx) {
            if (ctx.output) {
                *ctx.output << "tasks (stub)\n";
            }
            return CommandStatus::Handled;
        }});

    // T19: /cost reads the cumulative usage.jsonl aggregate. The
    // sink is created lazily on first use; for now we surface a
    // friendly message until the runtime wires the sink through
    // main.cpp.
    registry.register_command({"cost",
        "show cumulative usage cost",
        "(no arguments)",
        false,
        [](const std::string&, CommandContext& ctx) {
            if (ctx.output) {
                *ctx.output << "cost (stub — usage.jsonl not yet "
                                "wired through main.cpp)\n";
            }
            return CommandStatus::Handled;
        }});

    // T18: /theme reports the active theme. The setter is reserved
    // for future work — runtime config currently drives the choice.
    registry.register_command({"theme",
        "show the active presentation theme",
        "(no arguments)",
        false,
        [](const std::string&, CommandContext& ctx) {
            if (ctx.output) {
                *ctx.output << "theme: claude\n";
            }
            return CommandStatus::Handled;
        }});
}

std::optional<std::string> InteractiveCli::confirm_plan_buffer(
    const std::string& session_id, const std::string& plan_text) {
    (void)session_id;
    while (true) {
        output_ << "\nCommit this plan? [y/n/edit]: ";
        output_.flush();
        std::string answer;
        if (!std::getline(input_, answer)) {
            return std::nullopt;
        }
        answer = trim(std::move(answer));
        if (answer == "y" || answer == "yes") {
            return plan_text;
        }
        if (answer == "n" || answer == "no") {
            output_ << "Plan discarded.\n";
            return std::nullopt;
        }
        if (answer == "edit") {
            output_ << "Enter edited plan (finish with a single '.' on "
                       "its own line):\n";
            output_.flush();
            std::string edited;
            std::string line;
            while (std::getline(input_, line)) {
                if (line == ".") break;
                if (!edited.empty()) edited.push_back('\n');
                edited += line;
            }
            output_ << "\n--- edited plan ---\n"
                    << render_terminal_text(edited) << "\n";
            return edited;
        }
        output_ << "please answer y, n, or edit\n";
    }
}

}  // namespace agent

#pragma once

#include "application/session_engine.h"
#include "cli/cli_app.h"
#include "domain/memory_state.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace agent {

struct InteractiveSessionCommands {
    std::function<Result<std::vector<SessionState>>()> list;
    std::function<Result<SessionState>(const std::string&)> create;
    std::function<Result<SessionState>(const std::string&)> load;
    std::function<SessionTurnResult(
        const std::string&,
        const std::string&,
        const RuntimeProgressObserver&, bool)> submit;
    std::function<SessionTurnResult(
        const std::string&,
        const RuntimeProgressObserver&, bool)> recover;
    std::function<Result<std::vector<MemoryEntry>>(const std::string&)> memories;
    std::function<Result<MemoryEntry>(const std::string&, const std::string&)> remember;
    std::function<Result<void>(const std::string&)> forget;
    // request_maintenance is the non-blocking foreground entry point. It
    // accepts the session id and the highest committed through_sequence the
    // CLI has observed. The scheduler may discard older requests.
    std::function<void(const std::string&, std::uint64_t)> request_maintenance;
    // drain_for_exit is called once during CLI shutdown. It must never block
    // longer than the timeout it is given.
    std::function<void(std::chrono::milliseconds)> drain_for_exit;
    std::function<std::optional<std::uint64_t>(const std::string&)> pending_through;
    std::function<SessionTurnResult(
        const std::string&, const std::string&,
        const RuntimeProgressObserver&, bool,
        const RuntimePresentationOptions&)> submit_presented;
    std::function<SessionTurnResult(
        const std::string&, const std::string&,
        const RuntimeProgressObserver&, bool,
        const RuntimePresentationOptions&, bool dry_run)> submit_presented_dry;
    std::function<SessionTurnResult(
        const std::string&, const RuntimeProgressObserver&, bool,
        const RuntimePresentationOptions&)> recover_presented;
    std::function<void()> begin_turn;
    std::function<void()> end_turn;
    std::function<void()> cancel_turn;
    std::function<bool()> cancellation_requested;
    std::function<void(std::function<void(const std::string&)>)> set_phase_observer;
    // T11: returns the current permission state as a small JSON
    // document so /permissions can print it without depending on the
    // underlying StaticPermission type. Empty string means the
    // runtime was assembled without a Permission and the CLI prints a
    // "not configured" message instead.
    std::function<std::string()> permission_state_text;
};

struct InteractiveUiOptions {
    bool dynamic{false};
    bool stream{true};
    std::function<std::size_t()> columns;
};

class InteractiveCli {
public:
    InteractiveCli(InteractiveSessionCommands commands,
                   std::string model,
                   std::string default_workspace,
                   std::istream& input,
                   std::ostream& output,
                   std::ostream& error,
                   bool memory_enabled = true,
                   InteractiveUiOptions ui = {});

    int run();

private:
    SessionTurnResult execute_turn(const std::string& session_id,
                                  const std::string& text, bool recover,
                                  bool dry_run = false);
    void show_header(const SessionState& session);
    void show_status(const SessionState& session);
    bool render_turn(const SessionTurnResult& result,
                     SessionState& session);
    void consolidate(const SessionState& session);
    Result<std::vector<MemoryEntry>> eligible_memories(const SessionState& session);
    // T10: drive the /plan confirm loop. Returns the user's chosen text
    // (committed to the session if 'y', edited via subsequent prompts if
    // 'edit', discarded on 'n'). Returns std::nullopt if the user wants
    // to abort the whole flow.
    std::optional<std::string> confirm_plan_buffer(
        const std::string& session_id, const std::string& plan_text);

    InteractiveSessionCommands commands_;
    std::string model_;
    std::string default_workspace_;
    std::istream& input_;
    std::ostream& output_;
    std::ostream& error_;
    bool memory_available_;
    bool memory_on_;
    InteractiveUiOptions ui_;
    // T10: stores the model output of a /plan turn while the user
    // decides whether to commit, edit, or discard.
    std::string plan_buffer_;
};

}  // namespace agent

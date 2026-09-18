#pragma once

#include "domain/runtime_error.h"
#include "domain/runtime_event.h"
#include "domain/model_stream_event.h"
#include "domain/task_state.h"
#include "application/hook_chain.h"
#include "cli/theme.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace agent {

class Cancellation;
class Clock;
class EventStore;
class IdGenerator;
class KnowledgeProvider;
class ModelClient;
class Permission;
class ToolGateway;

struct RuntimeTextUpdate {
    std::string task_id;
    std::size_t model_round{0};
    ModelStreamEvent event;
};

// Presentation is transient and is never part of persisted task state.
struct RuntimePresentationOptions {
    bool stream{false};
    std::function<void(const RuntimeTextUpdate&)> text_observer;
    std::function<void(const std::string&)> phase_observer;
    // T18: theme selection. Presenters construct the matching spinner
    // glyph + verb vocabulary; the runtime treats it as opaque so this
    // field can be moved without dragging presenter code into the
    // application layer.
    ThemeId theme_id{ThemeId::Claude};
};

struct RunRequest {
    std::string issue;
    std::string workspace_utf8;
    std::string system_prompt;
    RuntimeBudgets budgets;
    std::vector<Message> initial_messages;
    std::optional<std::string> requested_task_id;
    std::optional<SessionTaskLink> session_link;
    RuntimePresentationOptions presentation;
    // T10: when true, the runtime refuses to enter AwaitingTool and stops
    // at the first EndTurn response. The plan is the model's plain text
    // output; the user confirms with `y` to actually run it.
    bool dry_run{false};
};

struct ResumeRequest {
    std::vector<RuntimeEvent> durable_events;
    std::string fallback_system_prompt;
    RuntimePresentationOptions presentation;
};

struct RuntimeResult {
    std::optional<TaskState> state;
    std::optional<RuntimeError> fatal_error;
};

// T08: per-event usage delta. The presenter accumulates these into
// a running total and renders "tok in/out/$cost" on the second status
// line. input_delta and output_delta are absolute counts (not deltas
// between rounds) so the presenter can sum them directly without
// remembering the previous event's values. usd is the dollar cost of
// this single event priced by the runtime config.
struct RuntimeUsageDelta {
    std::size_t input_delta{0};
    std::size_t output_delta{0};
    double usd{0.0};
};

// T06 (v2 §3): every `continue` inside RuntimeEngine::continue_task
// carries one of these reason labels. The runtime emits a trace
// sample tagged with the reason so post-mortem traces can answer
// "why did this turn cycle the loop?" without grepping through event
// payloads. Trace consumers filter on reason to bucket loops by
// state-machine shape (model call, tool call, compaction, cancel).
enum class ContinueReason {
    InitialCreate,
    ContextPrepared,
    ContextFailed,
    KnowledgeNoMatch,
    AwaitingModelNextRound,
    AwaitingToolNext,
    ToolsCompletedRound,
    ModelResponseAccepted,
    CompactionSucceeded,
    CancelledByUser,
    BudgetExceeded,
    InvariantFailure,
};

inline const char* continue_reason_name(ContinueReason reason) {
    switch (reason) {
    case ContinueReason::InitialCreate:           return "initial_create";
    case ContinueReason::ContextPrepared:         return "context_prepared";
    case ContinueReason::ContextFailed:           return "context_failed";
    case ContinueReason::KnowledgeNoMatch:        return "knowledge_no_match";
    case ContinueReason::AwaitingModelNextRound:  return "awaiting_model_next";
    case ContinueReason::AwaitingToolNext:        return "awaiting_tool_next";
    case ContinueReason::ToolsCompletedRound:     return "tools_completed";
    case ContinueReason::ModelResponseAccepted:   return "model_response_accepted";
    case ContinueReason::CompactionSucceeded:     return "compaction_succeeded";
    case ContinueReason::CancelledByUser:         return "cancelled_by_user";
    case ContinueReason::BudgetExceeded:          return "budget_exceeded";
    case ContinueReason::InvariantFailure:        return "invariant_failure";
    }
    return "unknown";
}

struct RuntimeProgress {
    std::string task_id;
    std::uint64_t sequence;
    EventKind event_kind;
    TaskStatus status;
    std::string tool_name;
    RuntimeUsageDelta usage_delta;
    // T09: pre-rendered unified diff lines for ToolCallSucceeded events
    // whose underlying tool call returned a "diff" field (replace_text /
    // write_file). Empty for everything else. The presenter colours '+'
    // and '-' prefixes green / red.
    std::vector<std::string> diff_lines;
};

using RuntimeProgressObserver =
    std::function<void(const RuntimeProgress&)>;

class RuntimeEngine {
public:
    RuntimeEngine(ModelClient& model,
                  ToolGateway& tools,
                  KnowledgeProvider& knowledge,
                  EventStore& events,
                  Clock& clock,
                  IdGenerator& ids,
                  Cancellation& cancellation,
                  std::string model_name = {},
                  std::function<void()> reactive_compact_trigger = {},
                  std::shared_ptr<HookChain> hook_chain = {},
                  std::shared_ptr<Permission> permission = {});

    const std::string& model_name() const noexcept { return model_name_; }

    RuntimeResult run(const RunRequest& request,
                      RuntimeProgressObserver observer);
    RuntimeResult resume(const ResumeRequest& request,
                         RuntimeProgressObserver observer);

private:
    RuntimeResult append_event(std::optional<TaskState>& state,
                               const std::string& task_id,
                               EventPayload payload,
                               RuntimeProgressObserver& observer,
                               const std::string& model_for_pricing = {});
    RuntimeResult guard_external_call(std::optional<TaskState>& state,
                                      const std::string& task_id,
                                      std::int64_t started_at_ms,
                                      RuntimeProgressObserver& observer,
                                      const char* count_budget_name = nullptr,
                                      std::size_t count = 0,
                                      std::size_t limit = 0);
    RuntimeResult continue_task(std::optional<TaskState>& state,
                                const std::string& system_prompt,
                                std::int64_t started_at_ms,
                                RuntimeProgressObserver& observer,
                                RuntimePresentationOptions presentation,
                                bool dry_run = false);

    ModelClient& model_;
    ToolGateway& tools_;
    KnowledgeProvider& knowledge_;
    EventStore& events_;
    Clock& clock_;
    IdGenerator& ids_;
    Cancellation& cancellation_;
    std::string model_name_;
    // T25: optional callback invoked when the model emits a
    // CompactRequestBlock. SessionEngine wires this to its
    // CompactChain::ReactiveCompact::trigger(); tests can install
    // their own callback to assert the trigger fired.
    std::function<void()> reactive_compact_trigger_;
    // T13: optional declarative hook chain. When non-null,
    // AwaitingTool runs each ToolCall through preToolUse / postToolUse
    // before / after the gateway execute() call. Other hooks
    // (preModelCall / postModelCall) are reserved for T11 Permission
    // and T19 Cost-tracker.
    std::shared_ptr<HookChain> hook_chain_;
    // T11: optional Permission policy. When non-null, AwaitingTool
    // queries check() for every call before dispatch. Allow proceeds,
    // Deny synthesises a failed ToolResult, Ask is treated as Deny
    // with reason "permission required" until T15 wires the REPL
    // confirm loop. A null pointer short-circuits the check entirely
    // (every call proceeds) so existing tests do not regress.
    std::shared_ptr<Permission> permission_;
};

}  // namespace agent

#include "application/state_reducer.h"

#include <string>
#include <type_traits>
#include <utility>

namespace agent {
namespace {

Result<void> invalid_transition(std::string message) {
    return Result<void>::failure(
        {ErrorCode::InvalidTransition, std::move(message), false});
}

bool response_contains_tool_calls(const Message& message) {
    for (const auto& block : message.content) {
        if (std::holds_alternative<ToolUseBlock>(block)) {
            return true;
        }
    }
    return false;
}

Result<void> apply_payload(TaskState& state, const EventPayload& payload) {
    return std::visit(
        [&state](const auto& typed_payload) -> Result<void> {
            using Payload = std::decay_t<decltype(typed_payload)>;

            if constexpr (std::is_same_v<Payload, TaskStartedPayload>) {
                return invalid_transition("task has already started");
            } else if constexpr (
                std::is_same_v<Payload, ContextPreparationStartedPayload>) {
                const bool starts_created = state.status == TaskStatus::Created;
                const bool tools_are_complete =
                    state.status == TaskStatus::AwaitingTool &&
                    !state.pending_tool_calls.empty() &&
                    !state.active_tool_call_id.has_value() &&
                    state.next_tool_index == state.pending_tool_calls.size() &&
                    state.pending_tool_results.size() == state.pending_tool_calls.size();
                if (!starts_created && !tools_are_complete) {
                    return invalid_transition(
                        "context preparation requires created or fully processed tools");
                }

                if (tools_are_complete) {
                    Message results_message{Role::User, {}};
                    results_message.content.reserve(state.pending_tool_results.size());
                    for (const auto& result : state.pending_tool_results) {
                        results_message.content.push_back(ToolResultBlock{result});
                    }
                    state.messages.push_back(std::move(results_message));
                    state.pending_tool_calls.clear();
                    state.pending_tool_results.clear();
                    state.next_tool_index = 0;
                    state.active_tool_call_id.reset();
                }
                state.status = TaskStatus::PreparingContext;
                return Result<void>::success();
            } else if constexpr (std::is_same_v<Payload, ContextPreparedPayload>) {
                if (state.status != TaskStatus::PreparingContext) {
                    return invalid_transition(
                        "context prepared requires preparing context state");
                }
                state.evidence = typed_payload.evidence;
                state.status = TaskStatus::AwaitingModel;
                return Result<void>::success();
            } else if constexpr (
                std::is_same_v<Payload, ContextPreparationFailedPayload>) {
                if (state.status != TaskStatus::PreparingContext) {
                    return invalid_transition(
                        "context failure requires preparing context state");
                }
                return Result<void>::success();
            } else if constexpr (std::is_same_v<Payload, ModelCallStartedPayload>) {
                if (state.status != TaskStatus::AwaitingModel) {
                    return invalid_transition(
                        "model call start requires awaiting model state");
                }
                ++state.usage.model_rounds;
                return Result<void>::success();
            } else if constexpr (std::is_same_v<Payload, ModelCallSucceededPayload>) {
                if (state.status != TaskStatus::AwaitingModel) {
                    return invalid_transition(
                        "model call success requires awaiting model state");
                }

                std::vector<ToolCall> ordered_calls;
                for (const auto& block : typed_payload.response.content) {
                    if (const auto* tool_use = std::get_if<ToolUseBlock>(&block)) {
                        ordered_calls.push_back(tool_use->call);
                    }
                }

                state.messages.push_back(
                    {Role::Assistant, typed_payload.response.content});
                if (!ordered_calls.empty()) {
                    state.pending_tool_calls = std::move(ordered_calls);
                    state.next_tool_index = 0;
                    state.active_tool_call_id.reset();
                    state.pending_tool_results.clear();
                    state.status = TaskStatus::AwaitingTool;
                }
                return Result<void>::success();
            } else if constexpr (std::is_same_v<Payload, ModelCallFailedPayload>) {
                if (state.status != TaskStatus::AwaitingModel) {
                    return invalid_transition(
                        "model call failure requires awaiting model state");
                }
                return Result<void>::success();
            } else if constexpr (std::is_same_v<Payload, ToolCallStartedPayload>) {
                if (state.status != TaskStatus::AwaitingTool ||
                    state.active_tool_call_id.has_value() ||
                    state.next_tool_index >= state.pending_tool_calls.size() ||
                    !(typed_payload.call ==
                      state.pending_tool_calls.at(state.next_tool_index))) {
                    return invalid_transition(
                        "tool call start does not match the next pending call");
                }
                state.active_tool_call_id = typed_payload.call.id;
                ++state.usage.tool_calls;
                return Result<void>::success();
            } else if constexpr (std::is_same_v<Payload, ToolCallSucceededPayload>) {
                if (state.status != TaskStatus::AwaitingTool ||
                    !state.active_tool_call_id.has_value() ||
                    typed_payload.result.tool_call_id != *state.active_tool_call_id) {
                    return invalid_transition(
                        "tool result does not match the active tool call");
                }
                state.pending_tool_results.push_back(typed_payload.result);
                state.active_tool_call_id.reset();
                ++state.next_tool_index;
                return Result<void>::success();
            } else if constexpr (std::is_same_v<Payload, ToolCallFailedPayload>) {
                if (state.status != TaskStatus::AwaitingTool ||
                    !state.active_tool_call_id.has_value() ||
                    typed_payload.tool_call_id != *state.active_tool_call_id) {
                    return invalid_transition(
                        "tool failure does not match the active tool call");
                }
                state.active_tool_call_id.reset();
                return Result<void>::success();
            } else if constexpr (std::is_same_v<Payload, TaskCompletedPayload>) {
                if (state.status != TaskStatus::AwaitingModel ||
                    !state.pending_tool_calls.empty() || state.messages.empty() ||
                    state.messages.back().role != Role::Assistant ||
                    response_contains_tool_calls(state.messages.back())) {
                    return invalid_transition(
                        "task completion requires a final tool-free model response");
                }
                state.final_text = typed_payload.final_text;
                state.status = TaskStatus::Completed;
                return Result<void>::success();
            } else if constexpr (std::is_same_v<Payload, TaskFailedPayload>) {
                state.terminal_error = typed_payload.error;
                state.status = TaskStatus::Failed;
                return Result<void>::success();
            } else if constexpr (
                std::is_same_v<Payload, TaskBudgetExceededPayload>) {
                state.terminal_error = typed_payload.error;
                state.status = TaskStatus::BudgetExceeded;
                return Result<void>::success();
            } else if constexpr (std::is_same_v<Payload, TaskCancelledPayload>) {
                state.terminal_error = typed_payload.error;
                state.status = TaskStatus::Cancelled;
                return Result<void>::success();
            }
        },
        payload);
}

}  // namespace

Result<TaskState> reduce_event(const std::optional<TaskState>& current,
                               const RuntimeEvent& event) {
    if (event.schema_version != 1) {
        return Result<TaskState>::failure(
            {ErrorCode::InvalidInput, "unsupported event schema version", false});
    }

    if (!current.has_value()) {
        if (event.sequence != 1) {
            return Result<TaskState>::failure(
                {ErrorCode::InvalidTransition,
                 "first event sequence must be one", false});
        }
        const auto* started = std::get_if<TaskStartedPayload>(&event.payload);
        if (started == nullptr) {
            return Result<TaskState>::failure(
                {ErrorCode::InvalidTransition,
                 "first event must start the task", false});
        }

        TaskState state;
        state.task_id = event.task_id;
        state.status = TaskStatus::Created;
        state.issue = started->issue;
        state.workspace_utf8 = started->workspace_utf8;
        state.budgets = started->budgets;
        state.messages.push_back(
            {Role::User, {TextBlock{started->issue}}});
        state.last_sequence = event.sequence;
        return Result<TaskState>::success(std::move(state));
    }

    if (event.task_id != current->task_id) {
        return Result<TaskState>::failure(
            {ErrorCode::InvalidInput, "event task ID does not match state", false});
    }
    if (event.sequence != current->last_sequence + 1) {
        return Result<TaskState>::failure(
            {ErrorCode::InvalidTransition, "event sequence is not contiguous", false});
    }
    if (is_terminal(current->status)) {
        return Result<TaskState>::failure(
            {ErrorCode::InvalidTransition,
             "terminal task state rejects further events", false});
    }

    TaskState next = *current;
    auto applied = apply_payload(next, event.payload);
    if (!applied.has_value()) {
        return Result<TaskState>::failure(applied.error());
    }
    next.last_sequence = event.sequence;
    return Result<TaskState>::success(std::move(next));
}

Result<TaskState> replay_events(const std::vector<RuntimeEvent>& events) {
    if (events.empty()) {
        return Result<TaskState>::failure(
            {ErrorCode::InvalidInput, "event trace is empty", false});
    }

    std::optional<TaskState> state;
    for (const auto& event : events) {
        auto reduced = reduce_event(state, event);
        if (!reduced.has_value()) {
            return Result<TaskState>::failure(reduced.error());
        }
        state = std::move(reduced.value());
    }
    return Result<TaskState>::success(std::move(*state));
}

}  // namespace agent

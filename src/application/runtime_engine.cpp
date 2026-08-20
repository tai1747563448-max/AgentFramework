#include "application/runtime_engine.h"

#include "application/state_reducer.h"
#include "ports/cancellation.h"
#include "ports/clock.h"
#include "ports/event_store.h"
#include "ports/id_generator.h"
#include "ports/knowledge_provider.h"
#include "ports/model_client.h"
#include "ports/tool_gateway.h"

#include <string>
#include <utility>
#include <variant>

namespace agent {
namespace {

RuntimeError persistence_failure(const RuntimeError& error) {
    return {ErrorCode::PersistenceFailure, error.message, error.retryable};
}

bool contains_tool_call(const ModelResponse& response) {
    for (const auto& block : response.content) {
        if (std::holds_alternative<ToolUseBlock>(block)) {
            return true;
        }
    }
    return false;
}

bool contains_tool_result(const ModelResponse& response) {
    for (const auto& block : response.content) {
        if (std::holds_alternative<ToolResultBlock>(block)) {
            return true;
        }
    }
    return false;
}

std::string concatenate_text(const ModelResponse& response) {
    std::string final_text;
    for (const auto& block : response.content) {
        if (const auto* text = std::get_if<TextBlock>(&block)) {
            final_text += text->text;
        }
    }
    return final_text;
}

}  // namespace

RuntimeEngine::RuntimeEngine(ModelClient& model,
                             ToolGateway& tools,
                             KnowledgeProvider& knowledge,
                             EventStore& events,
                             Clock& clock,
                             IdGenerator& ids,
                             Cancellation& cancellation)
    : model_(model),
      tools_(tools),
      knowledge_(knowledge),
      events_(events),
      clock_(clock),
      ids_(ids),
      cancellation_(cancellation) {}

RuntimeResult RuntimeEngine::append_event(std::optional<TaskState>& state,
                                          const std::string& task_id,
                                          EventPayload payload,
                                          RuntimeProgressObserver& observer) {
    const std::uint64_t sequence =
        state.has_value() ? state->last_sequence + 1 : 1;
    RuntimeEvent event{1, sequence, task_id, clock_.now_utc(),
                       ids_.next_correlation_id(), std::move(payload)};

    auto reduced = reduce_event(state, event);
    if (!reduced.has_value()) {
        return {state, reduced.error()};
    }

    auto appended = events_.append(event);
    if (!appended.has_value()) {
        return {state, persistence_failure(appended.error())};
    }

    state = std::move(reduced.value());
    if (observer) {
        try {
            observer({state->task_id, state->last_sequence,
                      event_kind(event.payload), state->status});
        } catch (...) {
            observer = nullptr;
        }
    }
    return {state, std::nullopt};
}

RuntimeResult RuntimeEngine::guard_external_call(
    std::optional<TaskState>& state,
    const std::string& task_id,
    std::int64_t started_at_ms,
    RuntimeProgressObserver& observer,
    const char* count_budget_name,
    std::size_t count,
    std::size_t limit) {
    if (!state.has_value() || is_terminal(state->status)) {
        return {state, std::nullopt};
    }

    if (cancellation_.requested()) {
        const RuntimeError error{ErrorCode::Cancelled,
                                 "task cancellation requested", false};
        return append_event(
            state, task_id,
            TaskCancelledPayload{"cancellation requested", error}, observer);
    }

    if (clock_.monotonic_ms() - started_at_ms >=
        state->budgets.max_task_time_ms) {
        const RuntimeError error{ErrorCode::BudgetExceeded,
                                 "max_task_time_ms budget exceeded", false};
        return append_event(
            state, task_id,
            TaskBudgetExceededPayload{"max_task_time_ms", error}, observer);
    }

    if (count_budget_name != nullptr && count >= limit) {
        const RuntimeError error{
            ErrorCode::BudgetExceeded,
            std::string(count_budget_name) + " budget exceeded", false};
        return append_event(
            state, task_id,
            TaskBudgetExceededPayload{count_budget_name, error}, observer);
    }

    return {state, std::nullopt};
}

RuntimeResult RuntimeEngine::run(
    const RunRequest& request,
    RuntimeProgressObserver observer) {
    std::optional<TaskState> state;
    const std::int64_t started_at_ms = clock_.monotonic_ms();
    const std::string task_id = ids_.next_task_id();

    auto transition = append_event(
        state, task_id,
        TaskStartedPayload{request.issue, request.workspace_utf8, request.budgets},
        observer);
    if (transition.fatal_error.has_value()) {
        return transition;
    }

    while (!is_terminal(state->status)) {
        transition =
            append_event(state, task_id, ContextPreparationStartedPayload{},
                         observer);
        if (transition.fatal_error.has_value()) {
            return transition;
        }

        transition =
            guard_external_call(state, task_id, started_at_ms, observer);
        if (transition.fatal_error.has_value() || is_terminal(state->status)) {
            return transition;
        }

        auto evidence = knowledge_.retrieve(*state);
        if (!evidence.has_value()) {
            return append_event(
                state, task_id,
                ContextPreparationFailedPayload{evidence.error()}, observer);
        }

        transition = append_event(
            state, task_id, ContextPreparedPayload{std::move(evidence.value())},
            observer);
        if (transition.fatal_error.has_value()) {
            return transition;
        }

        transition = guard_external_call(
            state, task_id, started_at_ms, observer, "max_model_rounds",
            state->usage.model_rounds, state->budgets.max_model_rounds);
        if (transition.fatal_error.has_value() || is_terminal(state->status)) {
            return transition;
        }

        transition =
            guard_external_call(state, task_id, started_at_ms, observer);
        if (transition.fatal_error.has_value() || is_terminal(state->status)) {
            return transition;
        }
        auto definitions = tools_.definitions();

        ModelRequest model_request{request.system_prompt,
                                   state->messages,
                                   std::move(definitions),
                                   state->budgets.model_timeout_ms};
        model_request.evidence = state->evidence;

        transition = guard_external_call(
            state, task_id, started_at_ms, observer, "max_model_rounds",
            state->usage.model_rounds, state->budgets.max_model_rounds);
        if (transition.fatal_error.has_value() || is_terminal(state->status)) {
            return transition;
        }
        transition = append_event(
            state, task_id, ModelCallStartedPayload{model_request}, observer);
        if (transition.fatal_error.has_value()) {
            return transition;
        }

        auto response = model_.complete(model_request);
        if (!response.has_value()) {
            return append_event(state, task_id,
                                ModelCallFailedPayload{response.error()},
                                observer);
        }

        const auto protocol_failure = [&](const char* message) {
            return append_event(
                state, task_id,
                ModelCallFailedPayload{
                    {ErrorCode::ProtocolFailure, message, false}},
                observer);
        };
        if (contains_tool_result(response.value())) {
            return protocol_failure(
                "model response contains an invalid tool-result block");
        }

        const bool has_tool_call = contains_tool_call(response.value());
        const auto final_text = concatenate_text(response.value());

        switch (response.value().stop_reason) {
        case StopReason::Unknown:
            return protocol_failure("model returned an unknown stop reason");
        case StopReason::ToolUse:
            if (!has_tool_call) {
                return protocol_failure(
                    "tool-use stop did not contain a tool-use block");
            }
            break;
        case StopReason::EndTurn:
        case StopReason::StopSequence:
            if (has_tool_call || final_text.empty()) {
                return protocol_failure(
                    "terminal text stop requires nonempty text and no tools");
            }

            transition = append_event(
                state, task_id,
                ModelCallSucceededPayload{std::move(response.value())},
                observer);
            if (transition.fatal_error.has_value()) {
                return transition;
            }
            return append_event(
                state, task_id, TaskCompletedPayload{final_text}, observer);
        case StopReason::MaxTokens:
            if (has_tool_call) {
                return protocol_failure(
                    "max-tokens stop cannot contain tool-use blocks");
            }

            transition = append_event(
                state, task_id,
                ModelCallSucceededPayload{std::move(response.value())},
                observer);
            if (transition.fatal_error.has_value()) {
                return transition;
            }
            return append_event(
                state, task_id,
                TaskBudgetExceededPayload{
                    "max_tokens",
                    {ErrorCode::BudgetExceeded,
                     "model output token budget exceeded", false}},
                observer);
        }

        if (!has_tool_call) {
            return protocol_failure(
                "model returned no tool calls for a tool-use stop");
        }

        transition = append_event(
            state, task_id,
            ModelCallSucceededPayload{std::move(response.value())}, observer);
        if (transition.fatal_error.has_value()) {
            return transition;
        }

        while (state->next_tool_index < state->pending_tool_calls.size()) {
            transition = guard_external_call(
                state, task_id, started_at_ms, observer, "max_tool_calls",
                state->usage.tool_calls, state->budgets.max_tool_calls);
            if (transition.fatal_error.has_value() || is_terminal(state->status)) {
                return transition;
            }

            const ToolCall call =
                state->pending_tool_calls.at(state->next_tool_index);
            transition = append_event(
                state, task_id, ToolCallStartedPayload{call}, observer);
            if (transition.fatal_error.has_value()) {
                return transition;
            }

            auto tool_result = tools_.execute(call);
            if (!tool_result.has_value()) {
                return append_event(
                    state, task_id,
                    ToolCallFailedPayload{call.id, tool_result.error()},
                    observer);
            }

            transition = append_event(
                state, task_id,
                ToolCallSucceededPayload{std::move(tool_result.value())},
                observer);
            if (transition.fatal_error.has_value()) {
                return transition;
            }
        }
    }

    return {state, std::nullopt};
}

}  // namespace agent

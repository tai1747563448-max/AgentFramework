#include "application/state_reducer.h"

#include "domain/evidence_validation.h"

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

std::string concatenated_text(const std::vector<ContentBlock>& content) {
    std::string text;
    for (const auto& block : content) {
        if (const auto* text_block = std::get_if<TextBlock>(&block)) {
            text += text_block->text;
        }
    }
    return text;
}

Result<void> validate_model_response(const ModelResponse& response) {
    if (!is_known_stop_reason_pair(response.stop_reason,
                                   response.raw_stop_reason)) {
        return invalid_transition("model stop reason fields do not match");
    }
    if (!response_tool_uses_are_valid(response)) {
        return invalid_transition(
            "model response contains an invalid tool-use block");
    }
    if (!response_text_blocks_are_valid(response)) {
        return invalid_transition(
            "model response contains an invalid text block");
    }
    bool contains_tool_use = false;
    bool contains_tool_result = false;
    for (const auto& block : response.content) {
        contains_tool_use = contains_tool_use ||
                            std::holds_alternative<ToolUseBlock>(block);
        contains_tool_result = contains_tool_result ||
                               std::holds_alternative<ToolResultBlock>(block);
    }
    if (contains_tool_result) {
        return invalid_transition(
            "model response contains an invalid tool-result block");
    }
    switch (response.stop_reason) {
    case StopReason::ToolUse:
        if (!contains_tool_use) {
            return invalid_transition(
                "tool-use stop requires at least one tool-use block");
        }
        return Result<void>::success();
    case StopReason::EndTurn:
    case StopReason::StopSequence:
        if (contains_tool_use || concatenated_text(response.content).empty()) {
            return invalid_transition(
                "terminal text stop requires nonempty text and no tools");
        }
        return Result<void>::success();
    case StopReason::MaxTokens:
        if (contains_tool_use) {
            return invalid_transition(
                "max-tokens stop cannot contain tool-use blocks");
        }
        return Result<void>::success();
    case StopReason::Unknown:
        return invalid_transition("unknown model stop reason is not replayable");
    }
    return invalid_transition("unknown model stop reason is not replayable");
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
                state.accepted_model_stop_reason.reset();
                state.status = TaskStatus::PreparingContext;
                return Result<void>::success();
            } else if constexpr (std::is_same_v<Payload, ContextPreparedPayload>) {
                if (state.status != TaskStatus::PreparingContext) {
                    return invalid_transition(
                        "context prepared requires preparing context state");
                }
                if (!evidence_pack_is_valid(typed_payload.evidence)) {
                    return invalid_transition(
                        "context prepared evidence is invalid");
                }
                state.evidence = typed_payload.evidence;
                state.status = TaskStatus::AwaitingModel;
                return Result<void>::success();
            } else if constexpr (std::is_same_v<Payload, KnowledgeNoMatchPayload>) {
                const Message answer{
                    Role::Assistant, {TextBlock{typed_payload.final_text}}};
                auto completed_messages = state.messages;
                completed_messages.push_back(answer);
                if (state.status != TaskStatus::AwaitingModel ||
                    state.model_call_in_flight ||
                    !state.evidence.authoritative_no_match ||
                    typed_payload.final_text.empty() ||
                    !conversation_history_is_valid(completed_messages)) {
                    return invalid_transition(
                        "knowledge no-match requires authoritative empty evidence");
                }
                state.messages = std::move(completed_messages);
                state.final_text = typed_payload.final_text;
                state.status = TaskStatus::Completed;
                return Result<void>::success();
            } else if constexpr (
                std::is_same_v<Payload, ContextPreparationFailedPayload>) {
                if (state.status != TaskStatus::PreparingContext) {
                    return invalid_transition(
                        "context failure requires preparing context state");
                }
                state.terminal_error = typed_payload.error;
                state.status = TaskStatus::Failed;
                return Result<void>::success();
            } else if constexpr (std::is_same_v<Payload, ModelCallStartedPayload>) {
                if (state.status != TaskStatus::AwaitingModel ||
                    state.model_call_in_flight ||
                    state.accepted_model_stop_reason.has_value()) {
                    return invalid_transition(
                        "model call start requires idle awaiting model state");
                }
                if (typed_payload.request.messages != state.messages) {
                    return invalid_transition(
                        "model request messages do not match durable state");
                }
                if (!evidence_pack_is_valid(typed_payload.request.evidence) ||
                    !(typed_payload.request.evidence == state.evidence)) {
                    return invalid_transition(
                        "model request evidence does not match prepared context");
                }
                if (state.evidence.tool_use_forbidden &&
                    !typed_payload.request.tools.empty()) {
                    return invalid_transition(
                        "retrieval-backed model request cannot expose tools");
                }
                if (typed_payload.request.timeout_ms !=
                    state.budgets.model_timeout_ms) {
                    return invalid_transition(
                        "model request timeout does not match task budget");
                }
                if (state.last_model_request.has_value() &&
                    typed_payload.request.system_prompt !=
                        state.last_model_request->system_prompt) {
                    return invalid_transition(
                        "model request system prompt changed within the task");
                }
                state.model_call_in_flight = true;
                state.last_model_request = typed_payload.request;
                ++state.usage.model_rounds;
                return Result<void>::success();
            } else if constexpr (std::is_same_v<Payload, ModelCallSucceededPayload>) {
                if (state.status != TaskStatus::AwaitingModel ||
                    !state.model_call_in_flight) {
                    return invalid_transition(
                        "model call success requires an in-flight model call");
                }
                auto valid_response =
                    validate_model_response(typed_payload.response);
                if (!valid_response.has_value()) {
                    return valid_response;
                }

                std::vector<ToolCall> ordered_calls;
                for (const auto& block : typed_payload.response.content) {
                    if (const auto* tool_use = std::get_if<ToolUseBlock>(&block)) {
                        ordered_calls.push_back(tool_use->call);
                    }
                }
                if (state.evidence.tool_use_forbidden && !ordered_calls.empty()) {
                    return invalid_transition(
                        "retrieval evidence cannot authorize tool calls");
                }

                state.messages.push_back(
                    {Role::Assistant, typed_payload.response.content});
                state.model_call_in_flight = false;
                state.accepted_model_stop_reason =
                    typed_payload.response.stop_reason;
                if (!ordered_calls.empty()) {
                    state.pending_tool_calls = std::move(ordered_calls);
                    state.next_tool_index = 0;
                    state.active_tool_call_id.reset();
                    state.pending_tool_results.clear();
                    state.status = TaskStatus::AwaitingTool;
                }
                return Result<void>::success();
            } else if constexpr (std::is_same_v<Payload, ModelCallFailedPayload>) {
                if (state.status != TaskStatus::AwaitingModel ||
                    !state.model_call_in_flight) {
                    return invalid_transition(
                        "model call failure requires an in-flight model call");
                }
                state.model_call_in_flight = false;
                state.accepted_model_stop_reason.reset();
                state.terminal_error = typed_payload.error;
                state.status = TaskStatus::Failed;
                return Result<void>::success();
            } else if constexpr (std::is_same_v<Payload, ToolCallStartedPayload>) {
                // T01: allow batched ToolCallStarted so AwaitingTool can
                // dispatch a window of concurrency-safe tools in parallel
                // before any Succeeded arrives. The Started event matches
                // the next pending slot past the already-started-but-not-
                // yet-completed head; active_tool_call_id tracks the
                // most-recent Started call (still useful as a recovery
                // sentinel for the resume path).
                const std::size_t started_count =
                    state.next_tool_index + state.pending_tool_results.size();
                if (state.status != TaskStatus::AwaitingTool ||
                    started_count >= state.pending_tool_calls.size() ||
                    !(typed_payload.call ==
                      state.pending_tool_calls.at(started_count))) {
                    return invalid_transition(
                        "tool call start does not match the next pending call");
                }
                state.active_tool_call_id = typed_payload.call.id;
                ++state.usage.tool_calls;
                return Result<void>::success();
            } else if constexpr (std::is_same_v<Payload, ToolCallSucceededPayload>) {
                if (state.status != TaskStatus::AwaitingTool ||
                    state.next_tool_index >= state.pending_tool_calls.size() ||
                    typed_payload.result.tool_call_id !=
                        state.pending_tool_calls.at(state.next_tool_index).id) {
                    return invalid_transition(
                        "tool result does not match the next pending call");
                }
                state.pending_tool_results.push_back(typed_payload.result);
                ++state.next_tool_index;
                if (state.next_tool_index <
                    state.pending_tool_calls.size()) {
                    state.active_tool_call_id =
                        state.pending_tool_calls.at(state.next_tool_index).id;
                } else {
                    state.active_tool_call_id.reset();
                }
                return Result<void>::success();
            } else if constexpr (std::is_same_v<Payload, ToolCallFailedPayload>) {
                // T01 batched model: ToolCallFailed must still match the
                // head (next_tool_index). Failure short-circuits the
                // window; in-flight siblings' Started events stay on
                // disk but never receive a completion, which is the
                // intended fail-fast behaviour.
                if (state.status != TaskStatus::AwaitingTool ||
                    state.next_tool_index >= state.pending_tool_calls.size() ||
                    typed_payload.tool_call_id !=
                        state.pending_tool_calls.at(state.next_tool_index).id) {
                    return invalid_transition(
                        "tool failure does not match the next pending call");
                }
                state.active_tool_call_id.reset();
                state.terminal_error = typed_payload.error;
                state.status = TaskStatus::Failed;
                return Result<void>::success();
            } else if constexpr (std::is_same_v<Payload, TaskCompletedPayload>) {
                if (state.status != TaskStatus::AwaitingModel ||
                    state.model_call_in_flight ||
                    !state.pending_tool_calls.empty() || state.messages.empty() ||
                    state.messages.back().role != Role::Assistant ||
                    response_contains_tool_calls(state.messages.back()) ||
                    !state.accepted_model_stop_reason.has_value() ||
                    (*state.accepted_model_stop_reason != StopReason::EndTurn &&
                     *state.accepted_model_stop_reason !=
                         StopReason::StopSequence) ||
                    typed_payload.final_text !=
                        concatenated_text(state.messages.back().content)) {
                    return invalid_transition(
                        "task completion must match the accepted terminal response");
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
                const bool follows_max_tokens =
                    state.accepted_model_stop_reason == StopReason::MaxTokens;
                const bool follows_text_completion =
                    state.accepted_model_stop_reason == StopReason::EndTurn ||
                    state.accepted_model_stop_reason == StopReason::StopSequence;
                if (typed_payload.budget_name == "max_tokens") {
                    const RuntimeError expected_error{
                        ErrorCode::BudgetExceeded,
                        "model output token budget exceeded", false};
                    if (state.status != TaskStatus::AwaitingModel ||
                        state.model_call_in_flight || !follows_max_tokens ||
                        state.messages.empty() ||
                        state.messages.back().role != Role::Assistant ||
                        response_contains_tool_calls(state.messages.back()) ||
                        !(typed_payload.error == expected_error)) {
                        return invalid_transition(
                            "task budget terminal does not match accepted response");
                    }
                } else {
                    const bool is_time_budget =
                        typed_payload.budget_name == "max_task_time_ms";
                    const bool is_model_budget =
                        typed_payload.budget_name == "max_model_rounds";
                    const bool is_tool_budget =
                        typed_payload.budget_name == "max_tool_calls";
                    const RuntimeError expected_error{
                        ErrorCode::BudgetExceeded,
                        typed_payload.budget_name + " budget exceeded", false};
                    if ((!is_time_budget && !is_model_budget && !is_tool_budget) ||
                        !(typed_payload.error == expected_error) ||
                        (!is_time_budget &&
                         (state.model_call_in_flight ||
                          state.active_tool_call_id.has_value())) ||
                        follows_max_tokens || follows_text_completion) {
                        return invalid_transition(
                            "task budget terminal is not a legal generic guard");
                    }

                    if (is_time_budget) {
                        const bool legal_status =
                            state.status == TaskStatus::PreparingContext ||
                            state.status == TaskStatus::AwaitingModel ||
                            (state.status == TaskStatus::AwaitingTool &&
                             state.next_tool_index <
                                 state.pending_tool_calls.size());
                        if (!legal_status) {
                            return invalid_transition(
                                "time budget requires an idle external-call guard");
                        }
                    } else if (is_model_budget) {
                        if (state.status != TaskStatus::AwaitingModel ||
                            state.accepted_model_stop_reason.has_value() ||
                            state.usage.model_rounds <
                                state.budgets.max_model_rounds) {
                            return invalid_transition(
                                "model-round budget requires an exhausted idle guard");
                        }
                    } else if (state.status != TaskStatus::AwaitingTool ||
                               state.accepted_model_stop_reason !=
                                   StopReason::ToolUse ||
                               state.next_tool_index >=
                                   state.pending_tool_calls.size() ||
                               state.usage.tool_calls <
                                   state.budgets.max_tool_calls) {
                        return invalid_transition(
                            "tool-call budget requires exhausted pending work");
                    }
                }
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
        if (!is_valid_task_id(event.task_id)) {
            return Result<TaskState>::failure(
                {ErrorCode::InvalidInput, "invalid task ID", false});
        }
        if (!has_positive_runtime_budgets(started->budgets)) {
            return Result<TaskState>::failure(
                {ErrorCode::InvalidInput,
                 "runtime budgets must be positive", false});
        }
        if (!conversation_history_is_valid(started->initial_messages)) {
            return Result<TaskState>::failure(
                {ErrorCode::InvalidInput,
                 "initial conversation history is invalid", false});
        }
        if (!started->initial_messages.empty() &&
            !started->session_link.has_value()) {
            return Result<TaskState>::failure(
                {ErrorCode::InvalidInput,
                 "initial conversation history requires a session link",
                 false});
        }
        if (started->session_link.has_value() &&
            (!is_valid_session_id(started->session_link->session_id) ||
             started->session_link->turn_index == 0)) {
            return Result<TaskState>::failure(
                {ErrorCode::InvalidInput,
                 "task session link is invalid", false});
        }

        TaskState state;
        state.task_id = event.task_id;
        state.status = TaskStatus::Created;
        state.issue = started->issue;
        state.workspace_utf8 = started->workspace_utf8;
        state.session_link = started->session_link;
        state.budgets = started->budgets;
        state.messages = started->initial_messages;
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

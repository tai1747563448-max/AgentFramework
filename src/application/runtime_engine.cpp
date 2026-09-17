#include "application/runtime_engine.h"

#include "application/state_reducer.h"
#include "domain/evidence_validation.h"
#include "domain/latency_trace.h"
#include "ports/cancellation.h"
#include "ports/operation_context.h"
#include "ports/clock.h"
#include "ports/event_store.h"
#include "ports/id_generator.h"
#include "ports/knowledge_provider.h"
#include "ports/model_client.h"
#include "ports/tool_gateway.h"

#include <string>
#include <cstdint>
#include <chrono>
#include <utility>
#include <variant>

namespace agent {
namespace {

RuntimeError persistence_failure(const RuntimeError& error) {
    return {ErrorCode::PersistenceFailure, error.message, error.retryable};
}

std::string progress_tool_name(const std::optional<TaskState>& state,
                               const EventPayload& payload) {
    if (const auto* started = std::get_if<ToolCallStartedPayload>(&payload)) {
        return started->call.name;
    }
    std::string call_id;
    if (const auto* succeeded = std::get_if<ToolCallSucceededPayload>(&payload)) {
        call_id = succeeded->result.tool_call_id;
    } else if (const auto* failed = std::get_if<ToolCallFailedPayload>(&payload)) {
        call_id = failed->tool_call_id;
    }
    if (state && !call_id.empty()) {
        for (const auto& call : state->pending_tool_calls) {
            if (call.id == call_id) return call.name;
        }
    }
    return {};
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

std::string concatenate_text_blocks(
    const std::vector<ContentBlock>& content) {
    std::string final_text;
    for (const auto& block : content) {
        if (const auto* text = std::get_if<TextBlock>(&block)) {
            final_text += text->text;
        }
    }
    return final_text;
}

std::string concatenate_text(const ModelResponse& response) {
    return concatenate_text_blocks(response.content);
}

bool contains_cjk(const std::string& text) {
    for (std::size_t index = 0; index < text.size();) {
        const auto first = static_cast<unsigned char>(text[index]);
        std::uint32_t codepoint = first;
        std::size_t width = 1;
        if ((first & 0xE0U) == 0xC0U && index + 1 < text.size()) {
            codepoint = static_cast<std::uint32_t>(first & 0x1FU) << 6U;
            width = 2;
        } else if ((first & 0xF0U) == 0xE0U && index + 2 < text.size()) {
            codepoint = static_cast<std::uint32_t>(first & 0x0FU) << 12U;
            width = 3;
        } else if ((first & 0xF8U) == 0xF0U && index + 3 < text.size()) {
            codepoint = static_cast<std::uint32_t>(first & 0x07U) << 18U;
            width = 4;
        }
        for (std::size_t offset = 1; offset < width; ++offset) {
            codepoint |= static_cast<std::uint32_t>(
                             static_cast<unsigned char>(text[index + offset]) &
                             0x3FU)
                         << (6U * static_cast<unsigned int>(width - offset - 1));
        }
        if ((codepoint >= 0x3400U && codepoint <= 0x4DBFU) ||
            (codepoint >= 0x4E00U && codepoint <= 0x9FFFU) ||
            (codepoint >= 0xF900U && codepoint <= 0xFAFFU) ||
            (codepoint >= 0x20000U && codepoint <= 0x323AFU)) {
            return true;
        }
        index += width;
    }
    return false;
}

std::string authoritative_no_match_text(const std::string& issue) {
    if (contains_cjk(issue)) {
        return u8"已配置的知识库快照中没有找到所请求权威条目的支持证据，因此未生成答案或引文。请核对条目编号，或将该资料加入知识库后重建索引。";
    }
    return "The configured Knowledge Pack contains no supporting evidence for "
           "the requested authoritative reference, so no answer or citation "
           "was generated. Check the reference or rebuild the index after "
           "adding the source.";
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
    const auto tool_name = observer ? progress_tool_name(state, event.payload)
                                    : std::string{};

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
                      event_kind(event.payload), state->status, tool_name});
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
    const std::string task_id = request.requested_task_id.has_value()
                                    ? *request.requested_task_id
                                    : ids_.next_task_id();
    if (!is_valid_task_id(task_id)) {
        return {std::nullopt,
                RuntimeError{ErrorCode::InvalidInput,
                             "requested task ID is invalid", false}};
    }

    auto transition = append_event(
        state, task_id,
        TaskStartedPayload{request.issue, request.workspace_utf8,
                           request.budgets, request.initial_messages,
                           request.session_link},
        observer);
    if (transition.fatal_error.has_value()) {
        return transition;
    }

    return continue_task(state, request.system_prompt, started_at_ms,
                         observer, request.presentation);
}

RuntimeResult RuntimeEngine::resume(
    const ResumeRequest& request,
    RuntimeProgressObserver observer) {
    auto replayed = replay_events(request.durable_events);
    if (!replayed.has_value()) {
        return {std::nullopt, replayed.error()};
    }

    std::optional<TaskState> state{std::move(replayed.value())};
    if (is_terminal(state->status)) {
        return {std::move(state), std::nullopt};
    }

    const std::string system_prompt =
        state->last_model_request.has_value()
            ? state->last_model_request->system_prompt
            : request.fallback_system_prompt;
    const std::int64_t started_at_ms = clock_.monotonic_ms();
    return continue_task(state, system_prompt, started_at_ms, observer,
                         request.presentation);
}

RuntimeResult RuntimeEngine::continue_task(
    std::optional<TaskState>& state,
    const std::string& system_prompt,
    std::int64_t started_at_ms,
    RuntimeProgressObserver& observer,
    RuntimePresentationOptions presentation) {
    const auto invariant_failure = [&](const char* message) {
        return RuntimeResult{
            state,
            RuntimeError{ErrorCode::InvalidTransition, message, false}};
    };

    while (!is_terminal(state->status)) {
        const std::string task_id = state->task_id;

        if (state->status == TaskStatus::Created) {
            auto transition = append_event(
                state, task_id, ContextPreparationStartedPayload{}, observer);
            if (transition.fatal_error.has_value()) {
                return transition;
            }
            continue;
        }

        if (state->status == TaskStatus::PreparingContext) {
            auto transition =
                guard_external_call(state, task_id, started_at_ms, observer);
            if (transition.fatal_error.has_value() ||
                is_terminal(state->status)) {
                return transition;
            }

            // T0 latency trace hook: emit the submit sample the moment the
            // turn's evidence retrieval is about to start. The task id is
            // used as the request id so all four canonical samples line up.
            emit_latency_sample(task_id, kStageSubmit);
            // T2: build an OperationContext that combines the runtime
            // cancellation token with a deadline derived from the task
            // budget. The provider uses both to short-circuit long-running
            // retrieval and stop spinning while the user already pressed
            // Ctrl+C.
            const OperationContext context{
                &cancellation_,
                std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(state->budgets.max_task_time_ms)};
            auto evidence = knowledge_.retrieve(*state, context);
            if (!evidence.has_value()) {
                return append_event(
                    state, task_id,
                    ContextPreparationFailedPayload{evidence.error()},
                    observer);
            }
            if (!evidence_pack_is_valid(evidence.value())) {
                return append_event(
                    state, task_id,
                    ContextPreparationFailedPayload{
                        {ErrorCode::ProtocolFailure,
                         "knowledge provider returned invalid evidence",
                         false}},
                    observer);
            }

            transition = append_event(
                state, task_id,
                ContextPreparedPayload{std::move(evidence.value())}, observer);
            if (transition.fatal_error.has_value()) {
                return transition;
            }
            continue;
        }

        if (state->status == TaskStatus::AwaitingModel) {
            if (state->evidence.authoritative_no_match) {
                return append_event(
                    state, task_id,
                    KnowledgeNoMatchPayload{
                        authoritative_no_match_text(state->issue)},
                    observer);
            }
            if (!state->model_call_in_flight &&
                state->accepted_model_stop_reason.has_value()) {
                switch (*state->accepted_model_stop_reason) {
                case StopReason::EndTurn:
                case StopReason::StopSequence:
                    if (state->messages.empty() ||
                        state->messages.back().role != Role::Assistant) {
                        return invariant_failure(
                            "accepted terminal response is unavailable");
                    }
                    return append_event(
                        state, task_id,
                        TaskCompletedPayload{concatenate_text_blocks(
                            state->messages.back().content)},
                        observer);
                case StopReason::MaxTokens:
                    return append_event(
                        state, task_id,
                        TaskBudgetExceededPayload{
                            "max_tokens",
                            {ErrorCode::BudgetExceeded,
                             "model output token budget exceeded", false}},
                        observer);
                case StopReason::ToolUse:
                case StopReason::Unknown:
                    return invariant_failure(
                        "accepted model response cannot be continued");
                }
            }

            ModelRequest model_request;
            if (state->model_call_in_flight) {
                if (!state->last_model_request.has_value()) {
                    return invariant_failure(
                        "in-flight model request is unavailable");
                }
                auto transition = guard_external_call(
                    state, task_id, started_at_ms, observer);
                if (transition.fatal_error.has_value() ||
                    is_terminal(state->status)) {
                    return transition;
                }
                model_request = *state->last_model_request;
            } else {
                auto transition = guard_external_call(
                    state, task_id, started_at_ms, observer,
                    "max_model_rounds", state->usage.model_rounds,
                    state->budgets.max_model_rounds);
                if (transition.fatal_error.has_value() ||
                    is_terminal(state->status)) {
                    return transition;
                }

                transition = guard_external_call(
                    state, task_id, started_at_ms, observer);
                if (transition.fatal_error.has_value() ||
                    is_terminal(state->status)) {
                    return transition;
                }
                auto definitions = tools_.definitions();
                if (state->evidence.tool_use_forbidden) {
                    // Retrieved articles are untrusted data. A model may quote or
                    // summarize them, but it cannot turn their contents into an
                    // executable capability request.
                    definitions.clear();
                }

                model_request = ModelRequest{
                    system_prompt, state->messages, std::move(definitions),
                    state->budgets.model_timeout_ms, state->evidence};

                transition = guard_external_call(
                    state, task_id, started_at_ms, observer,
                    "max_model_rounds", state->usage.model_rounds,
                    state->budgets.max_model_rounds);
                if (transition.fatal_error.has_value() ||
                    is_terminal(state->status)) {
                    return transition;
                }
                transition = append_event(
                    state, task_id,
                    ModelCallStartedPayload{model_request}, observer);
                if (transition.fatal_error.has_value()) {
                    return transition;
                }
            }

            ModelCallOptions options;
            options.stream = presentation.stream;
            options.cancellation = &cancellation_;
            // T0 latency trace: thread the task id into the cpr transport so
            // the first_text_received and request_send samples can be matched
            // with the submit and first_text_rendered samples of the same
            // turn. The transport ignores the field when it is empty.
            options.latency_request_id = task_id;
            // Evidence-backed responses are released only by the caller after
            // task completion and a successful session commit.
            if (presentation.stream && presentation.text_observer &&
                model_request.evidence.items.empty() &&
                !model_request.evidence.tool_use_forbidden &&
                !model_request.evidence.authoritative_no_match) {
                const auto round = state->usage.model_rounds;
                options.observer = [&, task_id, round](const ModelStreamEvent& event) {
                    if (!presentation.text_observer) return;
                    try {
                        presentation.text_observer({task_id, round, event});
                    } catch (...) {
                        presentation.text_observer = nullptr;
                    }
                };
            }
            auto response = model_.complete(model_request, options);
            // A transport may report cancellation without sharing this flag;
            // preserve its classification even if the wall budget also expired.
            if (!response.has_value() && response.error().code == ErrorCode::Cancelled) {
                return append_event(
                    state, task_id,
                    TaskCancelledPayload{"model request cancelled", response.error()},
                    observer);
            }
            auto after_model = guard_external_call(
                state, task_id, started_at_ms, observer);
            if (after_model.fatal_error.has_value() || is_terminal(state->status)) {
                return after_model;
            }
            if (!response.has_value()) {
                return append_event(
                    state, task_id,
                    ModelCallFailedPayload{response.error()}, observer);
            }

            const auto protocol_failure = [&](const char* message) {
                return append_event(
                    state, task_id,
                    ModelCallFailedPayload{
                        {ErrorCode::ProtocolFailure, message, false}},
                    observer);
            };
            if (!is_known_stop_reason_pair(
                    response.value().stop_reason,
                    response.value().raw_stop_reason)) {
                return protocol_failure(
                    "model stop reason fields do not match");
            }
            if (!response_text_blocks_are_valid(response.value())) {
                return protocol_failure(
                    "model response contains an empty text block");
            }
            if (!response_tool_uses_are_valid(response.value())) {
                return protocol_failure(
                    "model response contains an invalid tool-use block");
            }
            if (contains_tool_result(response.value())) {
                return protocol_failure(
                    "model response contains an invalid tool-result block");
            }

            const bool has_tool_call = contains_tool_call(response.value());
            const auto final_text = concatenate_text(response.value());
            if (has_tool_call && state->evidence.tool_use_forbidden) {
                return protocol_failure(
                    "model tool use is forbidden while retrieval evidence is active");
            }

            switch (response.value().stop_reason) {
            case StopReason::Unknown:
                return protocol_failure(
                    "model returned an unknown stop reason");
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

                break;
            case StopReason::MaxTokens:
                if (has_tool_call) {
                    return protocol_failure(
                        "max-tokens stop cannot contain tool-use blocks");
                }

                break;
            }

            // The last cancellation/time check is the response acceptance
            // point. Cancellation after the durable success does not roll it back.
            auto transition = guard_external_call(
                state, task_id, started_at_ms, observer);
            if (transition.fatal_error.has_value() || is_terminal(state->status)) {
                return transition;
            }
            const auto stop_reason = response.value().stop_reason;
            transition = append_event(
                state, task_id,
                ModelCallSucceededPayload{std::move(response.value())},
                observer);
            if (transition.fatal_error.has_value()) {
                return transition;
            }
            if (stop_reason == StopReason::EndTurn ||
                stop_reason == StopReason::StopSequence) {
                return append_event(state, task_id,
                    TaskCompletedPayload{final_text}, observer);
            }
            if (stop_reason == StopReason::MaxTokens) {
                return append_event(state, task_id,
                    TaskBudgetExceededPayload{
                        "max_tokens", {ErrorCode::BudgetExceeded,
                        "model output token budget exceeded", false}}, observer);
            }
            continue;
        }

        if (state->status == TaskStatus::AwaitingTool) {
            if (!state->active_tool_call_id.has_value() &&
                state->next_tool_index == state->pending_tool_calls.size()) {
                auto transition = append_event(
                    state, task_id, ContextPreparationStartedPayload{},
                    observer);
                if (transition.fatal_error.has_value()) {
                    return transition;
                }
                continue;
            }

            if (state->next_tool_index >=
                state->pending_tool_calls.size()) {
                return invariant_failure(
                    "awaiting tool state has no pending call");
            }
            const ToolCall call =
                state->pending_tool_calls.at(state->next_tool_index);

            if (state->active_tool_call_id.has_value()) {
                if (*state->active_tool_call_id != call.id) {
                    return invariant_failure(
                        "active tool identity does not match pending call");
                }
                auto transition = guard_external_call(
                    state, task_id, started_at_ms, observer);
                if (transition.fatal_error.has_value() ||
                    is_terminal(state->status)) {
                    return transition;
                }
            } else {
                auto transition = guard_external_call(
                    state, task_id, started_at_ms, observer,
                    "max_tool_calls", state->usage.tool_calls,
                    state->budgets.max_tool_calls);
                if (transition.fatal_error.has_value() ||
                    is_terminal(state->status)) {
                    return transition;
                }

                transition = append_event(
                    state, task_id, ToolCallStartedPayload{call}, observer);
                if (transition.fatal_error.has_value()) {
                    return transition;
                }
            }

            auto tool_result = tools_.execute(
                call, ToolExecutionContext{state->workspace_utf8});
            if (!tool_result.has_value()) {
                return append_event(
                    state, task_id,
                    ToolCallFailedPayload{call.id, tool_result.error()},
                    observer);
            }
            if (tool_result.value().tool_call_id != call.id) {
                return append_event(
                    state, task_id,
                    ToolCallFailedPayload{
                        call.id,
                        {ErrorCode::ProtocolFailure,
                         "tool result ID does not match active tool call",
                         false}},
                    observer);
            }

            auto transition = append_event(
                state, task_id,
                ToolCallSucceededPayload{std::move(tool_result.value())},
                observer);
            if (transition.fatal_error.has_value()) {
                return transition;
            }
            continue;
        }

        return invariant_failure("runtime reached an unknown task state");
    }

    return {state, std::nullopt};
}

}  // namespace agent

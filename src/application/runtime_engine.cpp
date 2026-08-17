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
                                          EventPayload payload) {
    const std::uint64_t sequence =
        state.has_value() ? state->last_sequence + 1 : 1;
    RuntimeEvent event{1, sequence, task_id, clock_.now_utc(),
                       ids_.next_correlation_id(), std::move(payload)};

    auto appended = events_.append(event);
    if (!appended.has_value()) {
        return {state, persistence_failure(appended.error())};
    }

    auto reduced = reduce_event(state, event);
    if (!reduced.has_value()) {
        return {state, reduced.error()};
    }
    state = std::move(reduced.value());
    return {state, std::nullopt};
}

RuntimeResult RuntimeEngine::run(const RunRequest& request) {
    std::optional<TaskState> state;
    const std::string task_id = ids_.next_task_id();

    auto transition = append_event(
        state, task_id,
        TaskStartedPayload{request.issue, request.workspace_utf8, request.budgets});
    if (transition.fatal_error.has_value()) {
        return transition;
    }

    transition = append_event(state, task_id, ContextPreparationStartedPayload{});
    if (transition.fatal_error.has_value()) {
        return transition;
    }

    auto evidence = knowledge_.retrieve(*state);
    if (!evidence.has_value()) {
        return append_event(
            state, task_id,
            ContextPreparationFailedPayload{evidence.error()});
    }

    transition = append_event(
        state, task_id, ContextPreparedPayload{std::move(evidence.value())});
    if (transition.fatal_error.has_value()) {
        return transition;
    }

    ModelRequest model_request{request.system_prompt,
                               state->messages,
                               tools_.definitions(),
                               state->budgets.model_timeout_ms};
    model_request.evidence = state->evidence;
    transition = append_event(
        state, task_id, ModelCallStartedPayload{model_request});
    if (transition.fatal_error.has_value()) {
        return transition;
    }

    auto response = model_.complete(model_request);
    if (!response.has_value()) {
        return append_event(state, task_id,
                            ModelCallFailedPayload{response.error()});
    }

    if (contains_tool_call(response.value())) {
        return append_event(
            state, task_id,
            ModelCallSucceededPayload{std::move(response.value())});
    }

    auto final_text = concatenate_text(response.value());
    if (final_text.empty()) {
        return append_event(
            state, task_id,
            ModelCallFailedPayload{
                {ErrorCode::ProtocolFailure,
                 "model returned no tool calls and no final text", false}});
    }

    transition = append_event(
        state, task_id,
        ModelCallSucceededPayload{std::move(response.value())});
    if (transition.fatal_error.has_value()) {
        return transition;
    }
    return append_event(
        state, task_id, TaskCompletedPayload{std::move(final_text)});
}

}  // namespace agent

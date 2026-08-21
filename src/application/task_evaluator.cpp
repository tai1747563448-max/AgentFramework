#include "application/task_evaluator.h"

#include "application/state_reducer.h"

#include <limits>
#include <utility>
#include <variant>

namespace agent {
namespace {

bool checked_add(std::size_t& destination, std::size_t amount) noexcept {
    if (amount > std::numeric_limits<std::size_t>::max() - destination) {
        return false;
    }
    destination += amount;
    return true;
}

Result<TaskEvaluation> metric_overflow() {
    return Result<TaskEvaluation>::failure(
        {ErrorCode::InvalidInput, "task evaluation metrics overflow", false});
}

}  // namespace

Result<TaskEvaluation> evaluate_task_events(
    const std::vector<RuntimeEvent>& events) {
    auto replayed = replay_events(events);
    if (!replayed.has_value()) {
        return Result<TaskEvaluation>::failure(replayed.error());
    }

    const auto& state = replayed.value();
    TaskEvaluation evaluation;
    evaluation.task_id = state.task_id;
    evaluation.status = state.status;
    evaluation.model_rounds = state.usage.model_rounds;
    evaluation.tool_calls = state.usage.tool_calls;
    evaluation.last_sequence = state.last_sequence;

    for (const auto& event : events) {
        if (const auto* prepared =
                std::get_if<ContextPreparedPayload>(&event.payload)) {
            if (!checked_add(evaluation.evidence_rounds, 1) ||
                !checked_add(evaluation.evidence_items,
                             prepared->evidence.items.size())) {
                return metric_overflow();
            }
            continue;
        }
        if (const auto* started =
                std::get_if<ModelCallStartedPayload>(&event.payload)) {
            if (!started->request.evidence.items.empty() &&
                !checked_add(evaluation.model_requests_with_evidence, 1)) {
                return metric_overflow();
            }
            continue;
        }
        if (const auto* succeeded =
                std::get_if<ToolCallSucceededPayload>(&event.payload)) {
            if (succeeded->result.is_error &&
                !checked_add(evaluation.tool_error_results, 1)) {
                return metric_overflow();
            }
        }
    }

    evaluation.passed =
        state.status == TaskStatus::Completed &&
        state.final_text.has_value() && !state.final_text->empty() &&
        !state.terminal_error.has_value() && !state.model_call_in_flight &&
        !state.active_tool_call_id.has_value() &&
        state.pending_tool_calls.empty() &&
        state.pending_tool_results.empty();
    return Result<TaskEvaluation>::success(std::move(evaluation));
}

}  // namespace agent

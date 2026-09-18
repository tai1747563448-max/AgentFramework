#include "domain/task_state.h"

namespace agent {

const char* task_type_name(TaskType type) {
    switch (type) {
        case TaskType::LocalBash:         return "local_bash";
        case TaskType::LocalAgent:        return "local_agent";
        case TaskType::InProcessTeammate: return "in_process_teammate";
    }
    return "local_bash";
}

TaskType parse_task_type(std::string_view text) {
    if (text == "local_bash") return TaskType::LocalBash;
    if (text == "local_agent") return TaskType::LocalAgent;
    if (text == "in_process_teammate") return TaskType::InProcessTeammate;
    // Default to LocalBash so an unknown model-emitted value still
    // resolves to a sensible, host-runnable type rather than
    // crashing the dispatch path.
    return TaskType::LocalBash;
}

// T17 follow-up: stable string for TaskStatus used by /tasks JSON
// snapshots. The string matches the JSON event-store encoding so a
// human reading both sees the same status name; future opentelemetry
// exporters can take the same path.
const char* task_status_name(TaskStatus status) {
    switch (status) {
        case TaskStatus::Created:           return "created";
        case TaskStatus::PreparingContext:  return "preparing_context";
        case TaskStatus::AwaitingModel:     return "awaiting_model";
        case TaskStatus::AwaitingTool:      return "awaiting_tool";
        case TaskStatus::Completed:         return "completed";
        case TaskStatus::Failed:            return "failed";
        case TaskStatus::BudgetExceeded:    return "budget_exceeded";
        case TaskStatus::Cancelled:         return "cancelled";
    }
    return "unknown";
}

}  // namespace agent

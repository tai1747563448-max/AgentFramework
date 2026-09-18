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

}  // namespace agent

#pragma once

#include "domain/result.h"
#include "domain/runtime_event.h"
#include "domain/task_state.h"

#include <optional>
#include <vector>

namespace agent {

Result<TaskState> reduce_event(const std::optional<TaskState>& current,
                               const RuntimeEvent& event);
Result<TaskState> replay_events(const std::vector<RuntimeEvent>& events);

}  // namespace agent

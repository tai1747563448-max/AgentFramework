#pragma once

#include "domain/result.h"
#include "domain/runtime_event.h"
#include "domain/task_state.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace agent {

struct TaskEvaluation {
    std::string task_id;
    bool passed{false};
    TaskStatus status{TaskStatus::Created};
    std::size_t model_rounds{0};
    std::size_t tool_calls{0};
    std::size_t evidence_rounds{0};
    std::size_t evidence_items{0};
    std::size_t model_requests_with_evidence{0};
    std::size_t tool_error_results{0};
    std::uint64_t last_sequence{0};
};

Result<TaskEvaluation> evaluate_task_events(
    const std::vector<RuntimeEvent>& events);

}  // namespace agent

#pragma once

#include "domain/runtime_error.h"
#include "domain/runtime_event.h"
#include "domain/task_state.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace agent {

class Cancellation;
class Clock;
class EventStore;
class IdGenerator;
class KnowledgeProvider;
class ModelClient;
class ToolGateway;

struct RunRequest {
    std::string issue;
    std::string workspace_utf8;
    std::string system_prompt;
    RuntimeBudgets budgets;
};

struct RuntimeResult {
    std::optional<TaskState> state;
    std::optional<RuntimeError> fatal_error;
};

struct RuntimeProgress {
    std::string task_id;
    std::uint64_t sequence;
    EventKind event_kind;
    TaskStatus status;
};

using RuntimeProgressObserver =
    std::function<void(const RuntimeProgress&)>;

class RuntimeEngine {
public:
    RuntimeEngine(ModelClient& model,
                  ToolGateway& tools,
                  KnowledgeProvider& knowledge,
                  EventStore& events,
                  Clock& clock,
                  IdGenerator& ids,
                  Cancellation& cancellation);

    RuntimeResult run(const RunRequest& request,
                      RuntimeProgressObserver observer);

private:
    RuntimeResult append_event(std::optional<TaskState>& state,
                               const std::string& task_id,
                               EventPayload payload,
                               RuntimeProgressObserver& observer);
    RuntimeResult guard_external_call(std::optional<TaskState>& state,
                                      const std::string& task_id,
                                      std::int64_t started_at_ms,
                                      RuntimeProgressObserver& observer,
                                      const char* count_budget_name = nullptr,
                                      std::size_t count = 0,
                                      std::size_t limit = 0);

    ModelClient& model_;
    ToolGateway& tools_;
    KnowledgeProvider& knowledge_;
    EventStore& events_;
    Clock& clock_;
    IdGenerator& ids_;
    Cancellation& cancellation_;
};

}  // namespace agent

#pragma once

#include "domain/task_state.h"

#include <cstdint>
#include <string>
#include <variant>

namespace agent {

enum class EventKind {
    TaskStarted,
    ContextPreparationStarted,
    ContextPrepared,
    KnowledgeNoMatch,
    ContextPreparationFailed,
    ModelCallStarted,
    ModelCallSucceeded,
    ModelCallFailed,
    ToolCallStarted,
    ToolCallSucceeded,
    ToolCallFailed,
    TaskCompleted,
    TaskFailed,
    TaskBudgetExceeded,
    TaskCancelled
};

struct TaskStartedPayload {
    std::string issue;
    std::string workspace_utf8;
    RuntimeBudgets budgets;
    std::vector<Message> initial_messages;
    std::optional<SessionTaskLink> session_link;
};

struct ContextPreparationStartedPayload {};

struct ContextPreparedPayload {
    EvidencePack evidence;
};

struct KnowledgeNoMatchPayload {
    std::string final_text;
};

struct ContextPreparationFailedPayload {
    RuntimeError error;
};

struct ModelCallStartedPayload {
    ModelRequest request;
};

struct ModelCallSucceededPayload {
    ModelResponse response;
};

struct ModelCallFailedPayload {
    RuntimeError error;
};

struct ToolCallStartedPayload {
    ToolCall call;
};

struct ToolCallSucceededPayload {
    ToolResult result;
};

struct ToolCallFailedPayload {
    std::string tool_call_id;
    RuntimeError error;
};

struct TaskCompletedPayload {
    std::string final_text;
};

struct TaskFailedPayload {
    RuntimeError error;
};

struct TaskBudgetExceededPayload {
    std::string budget_name;
    RuntimeError error;
};

struct TaskCancelledPayload {
    std::string reason;
    RuntimeError error;
};

using EventPayload = std::variant<
    TaskStartedPayload,
    ContextPreparationStartedPayload, ContextPreparedPayload,
    KnowledgeNoMatchPayload,
    ContextPreparationFailedPayload,
    ModelCallStartedPayload, ModelCallSucceededPayload, ModelCallFailedPayload,
    ToolCallStartedPayload, ToolCallSucceededPayload, ToolCallFailedPayload,
    TaskCompletedPayload, TaskFailedPayload,
    TaskBudgetExceededPayload, TaskCancelledPayload>;

struct RuntimeEvent {
    std::uint32_t schema_version{1};
    std::uint64_t sequence{0};
    std::string task_id;
    std::string timestamp_utc;
    std::string correlation_id;
    EventPayload payload;
};

EventKind event_kind(const EventPayload& payload);

inline bool operator==(const TaskStartedPayload& left, const TaskStartedPayload& right) {
    return left.issue == right.issue && left.workspace_utf8 == right.workspace_utf8 &&
           left.budgets == right.budgets &&
           left.initial_messages == right.initial_messages &&
           left.session_link == right.session_link;
}

inline bool operator==(const ContextPreparationStartedPayload&,
                       const ContextPreparationStartedPayload&) {
    return true;
}

inline bool operator==(const ContextPreparedPayload& left,
                       const ContextPreparedPayload& right) {
    return left.evidence == right.evidence;
}

inline bool operator==(const KnowledgeNoMatchPayload& left,
                       const KnowledgeNoMatchPayload& right) {
    return left.final_text == right.final_text;
}

inline bool operator==(const ContextPreparationFailedPayload& left,
                       const ContextPreparationFailedPayload& right) {
    return left.error == right.error;
}

inline bool operator==(const ModelCallStartedPayload& left,
                       const ModelCallStartedPayload& right) {
    return left.request == right.request;
}

inline bool operator==(const ModelCallSucceededPayload& left,
                       const ModelCallSucceededPayload& right) {
    return left.response == right.response;
}

inline bool operator==(const ModelCallFailedPayload& left,
                       const ModelCallFailedPayload& right) {
    return left.error == right.error;
}

inline bool operator==(const ToolCallStartedPayload& left,
                       const ToolCallStartedPayload& right) {
    return left.call == right.call;
}

inline bool operator==(const ToolCallSucceededPayload& left,
                       const ToolCallSucceededPayload& right) {
    return left.result == right.result;
}

inline bool operator==(const ToolCallFailedPayload& left,
                       const ToolCallFailedPayload& right) {
    return left.tool_call_id == right.tool_call_id && left.error == right.error;
}

inline bool operator==(const TaskCompletedPayload& left,
                       const TaskCompletedPayload& right) {
    return left.final_text == right.final_text;
}

inline bool operator==(const TaskFailedPayload& left, const TaskFailedPayload& right) {
    return left.error == right.error;
}

inline bool operator==(const TaskBudgetExceededPayload& left,
                       const TaskBudgetExceededPayload& right) {
    return left.budget_name == right.budget_name && left.error == right.error;
}

inline bool operator==(const TaskCancelledPayload& left,
                       const TaskCancelledPayload& right) {
    return left.reason == right.reason && left.error == right.error;
}

inline bool operator==(const RuntimeEvent& left, const RuntimeEvent& right) {
    return left.schema_version == right.schema_version && left.sequence == right.sequence &&
           left.task_id == right.task_id && left.timestamp_utc == right.timestamp_utc &&
           left.correlation_id == right.correlation_id && left.payload == right.payload;
}

}  // namespace agent

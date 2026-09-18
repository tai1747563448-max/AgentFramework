#pragma once

#include "domain/model_types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agent {

enum class TaskStatus {
    Created,
    PreparingContext,
    AwaitingModel,
    AwaitingTool,
    Completed,
    Failed,
    BudgetExceeded,
    Cancelled
};

struct RuntimeBudgets {
    std::size_t max_model_rounds{16};
    std::size_t max_tool_calls{64};
    std::int64_t max_task_time_ms{1'800'000};
    std::int64_t model_timeout_ms{120'000};
    // T01: maximum number of pending tool calls AwaitingTool will
    // dispatch in a single pass. Read-only tools (concurrency_safe=true)
    // run in parallel via std::async; mutating tools run serially in the
    // same window. A value of 1 reproduces the pre-T01 single-call
    // behaviour. The field is appended at the end of RuntimeBudgets so
    // event_json.cpp's 4-arg aggregate initializer keeps mapping the
    // existing JSON fields to their original slots.
    std::size_t max_parallel_tools{4};
};

struct RuntimeUsage {
    std::size_t model_rounds{0};
    std::size_t tool_calls{0};
};

struct SessionTaskLink {
    std::string session_id;
    std::uint64_t turn_index{0};
};

inline bool operator==(const SessionTaskLink& left,
                       const SessionTaskLink& right) {
    return left.session_id == right.session_id &&
           left.turn_index == right.turn_index;
}

struct TaskState {
    std::string task_id;
    TaskStatus status{TaskStatus::Created};
    std::string issue;
    std::string workspace_utf8;
    std::optional<SessionTaskLink> session_link;
    RuntimeBudgets budgets;
    RuntimeUsage usage;
    std::uint64_t last_sequence{0};
    bool model_call_in_flight{false};
    std::optional<ModelRequest> last_model_request;
    std::optional<StopReason> accepted_model_stop_reason;
    std::vector<Message> messages;
    EvidencePack evidence;
    std::vector<ToolCall> pending_tool_calls;
    std::size_t next_tool_index{0};
    std::optional<std::string> active_tool_call_id;
    std::vector<ToolResult> pending_tool_results;
    std::optional<std::string> final_text;
    std::optional<RuntimeError> terminal_error;
};

inline bool is_terminal(TaskStatus status) noexcept {
    return status == TaskStatus::Completed || status == TaskStatus::Failed ||
           status == TaskStatus::BudgetExceeded || status == TaskStatus::Cancelled;
}

inline bool is_valid_task_id(std::string_view task_id) noexcept {
    constexpr std::string_view kPrefix = "task-";
    constexpr std::size_t kHexCharacters = 32;
    if (task_id.size() != kPrefix.size() + kHexCharacters ||
        task_id.substr(0, kPrefix.size()) != kPrefix) {
        return false;
    }
    for (const char character : task_id.substr(kPrefix.size())) {
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

inline bool is_valid_session_id(std::string_view session_id) noexcept {
    constexpr std::string_view kPrefix = "session-";
    constexpr std::size_t kHexCharacters = 32;
    if (session_id.size() != kPrefix.size() + kHexCharacters ||
        session_id.substr(0, kPrefix.size()) != kPrefix) {
        return false;
    }
    for (const char character : session_id.substr(kPrefix.size())) {
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

inline bool has_positive_runtime_budgets(
    const RuntimeBudgets& budgets) noexcept {
    return budgets.max_model_rounds > 0 && budgets.max_tool_calls > 0 &&
           budgets.max_parallel_tools > 0 &&
           budgets.max_task_time_ms > 0 && budgets.model_timeout_ms > 0;
}

inline bool operator==(const RuntimeBudgets& left, const RuntimeBudgets& right) {
    return left.max_model_rounds == right.max_model_rounds &&
           left.max_tool_calls == right.max_tool_calls &&
           left.max_parallel_tools == right.max_parallel_tools &&
           left.max_task_time_ms == right.max_task_time_ms &&
           left.model_timeout_ms == right.model_timeout_ms;
}

inline bool operator==(const RuntimeUsage& left, const RuntimeUsage& right) {
    return left.model_rounds == right.model_rounds && left.tool_calls == right.tool_calls;
}

inline bool operator==(const TaskState& left, const TaskState& right) {
    return left.task_id == right.task_id && left.status == right.status &&
           left.issue == right.issue && left.workspace_utf8 == right.workspace_utf8 &&
           left.session_link == right.session_link &&
           left.budgets == right.budgets && left.usage == right.usage &&
           left.last_sequence == right.last_sequence &&
           left.model_call_in_flight == right.model_call_in_flight &&
           left.last_model_request == right.last_model_request &&
           left.accepted_model_stop_reason == right.accepted_model_stop_reason &&
           left.messages == right.messages &&
           left.evidence == right.evidence &&
           left.pending_tool_calls == right.pending_tool_calls &&
           left.next_tool_index == right.next_tool_index &&
           left.active_tool_call_id == right.active_tool_call_id &&
           left.pending_tool_results == right.pending_tool_results &&
           left.final_text == right.final_text && left.terminal_error == right.terminal_error;
}

}  // namespace agent

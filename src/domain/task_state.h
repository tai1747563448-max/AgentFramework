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
};

struct RuntimeUsage {
    std::size_t model_rounds{0};
    std::size_t tool_calls{0};
};

struct TaskState {
    std::string task_id;
    TaskStatus status{TaskStatus::Created};
    std::string issue;
    std::string workspace_utf8;
    RuntimeBudgets budgets;
    RuntimeUsage usage;
    std::uint64_t last_sequence{0};
    bool model_call_in_flight{false};
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

inline bool has_positive_runtime_budgets(
    const RuntimeBudgets& budgets) noexcept {
    return budgets.max_model_rounds > 0 && budgets.max_tool_calls > 0 &&
           budgets.max_task_time_ms > 0 && budgets.model_timeout_ms > 0;
}

inline bool operator==(const RuntimeBudgets& left, const RuntimeBudgets& right) {
    return left.max_model_rounds == right.max_model_rounds &&
           left.max_tool_calls == right.max_tool_calls &&
           left.max_task_time_ms == right.max_task_time_ms &&
           left.model_timeout_ms == right.model_timeout_ms;
}

inline bool operator==(const RuntimeUsage& left, const RuntimeUsage& right) {
    return left.model_rounds == right.model_rounds && left.tool_calls == right.tool_calls;
}

inline bool operator==(const TaskState& left, const TaskState& right) {
    return left.task_id == right.task_id && left.status == right.status &&
           left.issue == right.issue && left.workspace_utf8 == right.workspace_utf8 &&
           left.budgets == right.budgets && left.usage == right.usage &&
           left.last_sequence == right.last_sequence &&
           left.model_call_in_flight == right.model_call_in_flight &&
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

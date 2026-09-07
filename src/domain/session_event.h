#pragma once

#include "domain/session_state.h"

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace agent {

enum class SessionEventKind {
    SessionStarted,
    TurnStarted,
    TurnCommitted,
    TurnFailed,
    SessionCompacted
};

struct SessionStartedPayload {
    std::string workspace_utf8;
    std::string model;
};

struct SessionTurnStartedPayload {
    std::uint64_t turn_index{0};
    std::string task_id;
    std::string user_text;
};

struct SessionTurnCommittedPayload {
    std::uint64_t turn_index{0};
    std::string task_id;
    std::vector<Message> messages;
};

struct SessionTurnFailedPayload {
    std::uint64_t turn_index{0};
    std::string task_id;
    TaskStatus status{TaskStatus::Failed};
    std::string summary;
};

struct SessionCompactedPayload {
    std::uint64_t compacted_through_turn{0};
    std::string summary;
};

inline const char* safe_session_failure_summary(TaskStatus status) noexcept {
    switch (status) {
    case TaskStatus::Failed:
        return "task failed";
    case TaskStatus::BudgetExceeded:
        return "task budget exceeded";
    case TaskStatus::Cancelled:
        return "task cancelled";
    default:
        return nullptr;
    }
}

using SessionEventPayload = std::variant<
    SessionStartedPayload,
    SessionTurnStartedPayload,
    SessionTurnCommittedPayload,
    SessionTurnFailedPayload,
    SessionCompactedPayload>;

struct SessionEvent {
    std::uint32_t schema_version{1};
    std::uint64_t sequence{0};
    std::string session_id;
    std::string timestamp_utc;
    std::string correlation_id;
    SessionEventPayload payload;
};

SessionEventKind session_event_kind(const SessionEventPayload& payload);

inline bool operator==(const SessionStartedPayload& left,
                       const SessionStartedPayload& right) {
    return left.workspace_utf8 == right.workspace_utf8 &&
           left.model == right.model;
}

inline bool operator==(const SessionTurnStartedPayload& left,
                       const SessionTurnStartedPayload& right) {
    return left.turn_index == right.turn_index &&
           left.task_id == right.task_id &&
           left.user_text == right.user_text;
}

inline bool operator==(const SessionTurnCommittedPayload& left,
                       const SessionTurnCommittedPayload& right) {
    return left.turn_index == right.turn_index &&
           left.task_id == right.task_id &&
           left.messages == right.messages;
}

inline bool operator==(const SessionTurnFailedPayload& left,
                       const SessionTurnFailedPayload& right) {
    return left.turn_index == right.turn_index &&
           left.task_id == right.task_id &&
           left.status == right.status && left.summary == right.summary;
}

inline bool operator==(const SessionCompactedPayload& left,
                       const SessionCompactedPayload& right) {
    return left.compacted_through_turn == right.compacted_through_turn &&
           left.summary == right.summary;
}

inline bool operator==(const SessionEvent& left,
                       const SessionEvent& right) {
    return left.schema_version == right.schema_version &&
           left.sequence == right.sequence &&
           left.session_id == right.session_id &&
           left.timestamp_utc == right.timestamp_utc &&
           left.correlation_id == right.correlation_id &&
           left.payload == right.payload;
}

}  // namespace agent

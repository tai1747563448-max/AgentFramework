#pragma once

#include "domain/task_state.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace agent {

struct PendingSessionTurn {
    std::uint64_t turn_index{0};
    std::string task_id;
    std::string user_text;
};

inline bool operator==(const PendingSessionTurn& left,
                       const PendingSessionTurn& right) {
    return left.turn_index == right.turn_index &&
           left.task_id == right.task_id &&
           left.user_text == right.user_text;
}

struct CommittedSessionTurn {
    std::uint64_t turn_index{0};
    std::string task_id;
    std::vector<Message> messages;
};

inline bool operator==(const CommittedSessionTurn& left,
                       const CommittedSessionTurn& right) {
    return left.turn_index == right.turn_index &&
           left.task_id == right.task_id && left.messages == right.messages;
}

struct SessionState {
    std::string session_id;
    std::string workspace_utf8;
    std::string model;
    std::uint64_t last_sequence{0};
    std::string last_timestamp_utc;
    std::uint64_t completed_turns{0};
    std::vector<CommittedSessionTurn> committed_turns;
    std::string summary;
    std::uint64_t compacted_through_turn{0};
    std::vector<Message> messages;
    std::optional<PendingSessionTurn> pending_turn;
    std::optional<std::string> last_task_id;
    std::optional<TaskStatus> last_task_status;
};

inline bool operator==(const SessionState& left, const SessionState& right) {
    return left.session_id == right.session_id &&
           left.workspace_utf8 == right.workspace_utf8 &&
           left.model == right.model &&
           left.last_sequence == right.last_sequence &&
           left.last_timestamp_utc == right.last_timestamp_utc &&
           left.completed_turns == right.completed_turns &&
           left.committed_turns == right.committed_turns &&
           left.summary == right.summary &&
           left.compacted_through_turn == right.compacted_through_turn &&
           left.messages == right.messages &&
           left.pending_turn == right.pending_turn &&
           left.last_task_id == right.last_task_id &&
           left.last_task_status == right.last_task_status;
}

}  // namespace agent

#include "application/session_reducer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace agent {
namespace {

Result<SessionState> invalid(ErrorCode code, const char* message) {
    return Result<SessionState>::failure({code, message, false});
}

bool matches_pending(const SessionState& state,
                     std::uint64_t turn_index,
                     const std::string& task_id) {
    return state.pending_turn.has_value() &&
           state.pending_turn->turn_index == turn_index &&
           state.pending_turn->task_id == task_id;
}

bool starts_with_pending_user(const std::vector<Message>& messages,
                              const PendingSessionTurn& pending) {
    if (messages.empty()) {
        return false;
    }
    const Message expected{
        Role::User, {TextBlock{pending.user_text}}};
    return messages.front() == expected;
}

bool is_strict_utf8_text(const std::string& text) {
    for (std::size_t offset = 0; offset < text.size();) {
        const auto first = static_cast<unsigned char>(text[offset]);
        if (first == 0) {
            return false;
        }
        std::size_t width = 0;
        std::uint32_t code_point = 0;
        if (first <= 0x7F) {
            width = 1;
            code_point = first;
        } else if (first >= 0xC2 && first <= 0xDF) {
            width = 2;
            code_point = first & 0x1F;
        } else if (first >= 0xE0 && first <= 0xEF) {
            width = 3;
            code_point = first & 0x0F;
        } else if (first >= 0xF0 && first <= 0xF4) {
            width = 4;
            code_point = first & 0x07;
        } else {
            return false;
        }
        if (offset + width > text.size()) {
            return false;
        }
        for (std::size_t index = 1; index < width; ++index) {
            const auto byte = static_cast<unsigned char>(text[offset + index]);
            if ((byte & 0xC0) != 0x80) {
                return false;
            }
            code_point = (code_point << 6) | (byte & 0x3F);
        }
        if ((width == 3 && code_point < 0x800) ||
            (width == 4 && code_point < 0x10000) ||
            (code_point >= 0xD800 && code_point <= 0xDFFF) ||
            code_point > 0x10FFFF) {
            return false;
        }
        offset += width;
    }
    return true;
}

std::vector<Message> flatten_committed_turns(
    const std::vector<CommittedSessionTurn>& turns) {
    std::vector<Message> messages;
    for (const auto& turn : turns) {
        messages.insert(messages.end(), turn.messages.begin(),
                        turn.messages.end());
    }
    return messages;
}

}  // namespace

Result<SessionState> reduce_session_event(
    const std::optional<SessionState>& current,
    const SessionEvent& event) {
    if (event.schema_version != 1) {
        return invalid(ErrorCode::InvalidInput,
                       "unsupported session event schema version");
    }
    if (!is_valid_session_id(event.session_id) ||
        event.timestamp_utc.empty() || event.correlation_id.empty()) {
        return invalid(ErrorCode::InvalidInput,
                       "session event identity is invalid");
    }

    if (!current.has_value()) {
        if (event.sequence != 1) {
            return invalid(ErrorCode::InvalidTransition,
                           "first session event sequence must be one");
        }
        const auto* started =
            std::get_if<SessionStartedPayload>(&event.payload);
        if (started == nullptr) {
            return invalid(ErrorCode::InvalidTransition,
                           "first session event must start the session");
        }
        if (started->workspace_utf8.empty() || started->model.empty()) {
            return invalid(ErrorCode::InvalidInput,
                           "session workspace and model must be nonempty");
        }
        SessionState state;
        state.session_id = event.session_id;
        state.workspace_utf8 = started->workspace_utf8;
        state.model = started->model;
        state.last_sequence = event.sequence;
        state.last_timestamp_utc = event.timestamp_utc;
        return Result<SessionState>::success(std::move(state));
    }

    if (event.session_id != current->session_id) {
        return invalid(ErrorCode::InvalidInput,
                       "session event ID does not match state");
    }
    if (event.sequence != current->last_sequence + 1) {
        return invalid(ErrorCode::InvalidTransition,
                       "session event sequence is not contiguous");
    }

    SessionState next = *current;
    const auto applied = std::visit(
        [&next](const auto& payload) -> Result<void> {
            using Payload = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<Payload,
                                         SessionStartedPayload>) {
                return Result<void>::failure(
                    {ErrorCode::InvalidTransition,
                     "session has already started", false});
            } else if constexpr (
                std::is_same_v<Payload, SessionTurnStartedPayload>) {
                if (next.pending_turn.has_value() ||
                    payload.turn_index != next.completed_turns + 1 ||
                    !is_valid_task_id(payload.task_id) ||
                    payload.user_text.empty()) {
                    return Result<void>::failure(
                        {ErrorCode::InvalidTransition,
                         "turn start is invalid for current session", false});
                }
                next.pending_turn = PendingSessionTurn{
                    payload.turn_index, payload.task_id,
                    payload.user_text};
                return Result<void>::success();
            } else if constexpr (
                std::is_same_v<Payload, SessionTurnCommittedPayload>) {
                if (!matches_pending(next, payload.turn_index,
                                     payload.task_id) ||
                    !starts_with_pending_user(
                        payload.messages, *next.pending_turn)) {
                    return Result<void>::failure(
                        {ErrorCode::InvalidTransition,
                         "turn commit does not match pending turn", false});
                }
                auto combined = next.messages;
                combined.insert(combined.end(), payload.messages.begin(),
                                payload.messages.end());
                if (!conversation_history_is_valid(combined)) {
                    return Result<void>::failure(
                        {ErrorCode::InvalidTransition,
                         "committed turn messages are invalid", false});
                }
                next.committed_turns.push_back(
                    {payload.turn_index, payload.task_id, payload.messages});
                next.messages = flatten_committed_turns(next.committed_turns);
                next.completed_turns = payload.turn_index;
                next.pending_turn.reset();
                next.last_task_id = payload.task_id;
                next.last_task_status = TaskStatus::Completed;
                return Result<void>::success();
            } else if constexpr (
                std::is_same_v<Payload, SessionTurnFailedPayload>) {
                const bool valid_status =
                    payload.status == TaskStatus::Failed ||
                    payload.status == TaskStatus::BudgetExceeded ||
                    payload.status == TaskStatus::Cancelled;
                const auto* expected_summary =
                    safe_session_failure_summary(payload.status);
                if (!matches_pending(next, payload.turn_index,
                                     payload.task_id) || !valid_status ||
                    expected_summary == nullptr ||
                    payload.summary != expected_summary) {
                    return Result<void>::failure(
                        {ErrorCode::InvalidTransition,
                         "turn failure does not match pending turn", false});
                }
                next.completed_turns = payload.turn_index;
                next.pending_turn.reset();
                next.last_task_id = payload.task_id;
                next.last_task_status = payload.status;
                return Result<void>::success();
            } else {
                if (next.pending_turn.has_value() ||
                    payload.compacted_through_turn <=
                        next.compacted_through_turn) {
                    return Result<void>::failure(
                        {ErrorCode::InvalidTransition,
                         "session compaction is invalid for current session",
                         false});
                }
                if (payload.summary.empty() ||
                    payload.summary.size() > 8192 ||
                    !is_strict_utf8_text(payload.summary)) {
                    return Result<void>::failure(
                        {ErrorCode::InvalidInput,
                         "session compaction summary is invalid", false});
                }
                const auto covered = std::find_if(
                    next.committed_turns.begin(), next.committed_turns.end(),
                    [&payload](const CommittedSessionTurn& turn) {
                        return turn.turn_index ==
                               payload.compacted_through_turn;
                    });
                if (covered == next.committed_turns.end() ||
                    std::next(covered) == next.committed_turns.end()) {
                    return Result<void>::failure(
                        {ErrorCode::InvalidTransition,
                         "session compaction coverage is invalid", false});
                }
                next.committed_turns.erase(next.committed_turns.begin(),
                                           std::next(covered));
                next.messages = flatten_committed_turns(next.committed_turns);
                next.summary = payload.summary;
                next.compacted_through_turn = payload.compacted_through_turn;
                return Result<void>::success();
            }
        },
        event.payload);
    if (!applied.has_value()) {
        return Result<SessionState>::failure(applied.error());
    }

    next.last_sequence = event.sequence;
    next.last_timestamp_utc = event.timestamp_utc;
    return Result<SessionState>::success(std::move(next));
}

Result<SessionState> replay_session_events(
    const std::vector<SessionEvent>& events) {
    if (events.empty()) {
        return invalid(ErrorCode::InvalidInput,
                       "session event trace is empty");
    }
    std::optional<SessionState> state;
    for (const auto& event : events) {
        auto reduced = reduce_session_event(state, event);
        if (!reduced.has_value()) {
            return reduced;
        }
        state = std::move(reduced.value());
    }
    return Result<SessionState>::success(std::move(*state));
}

}  // namespace agent

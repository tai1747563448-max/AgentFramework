#include "application/memory_reducer.h"
#include <limits>
#include <type_traits>

namespace agent {
namespace {
Result<MemoryState> invalid_transition() {
    return Result<MemoryState>::failure(
        {ErrorCode::InvalidTransition, "memory event transition is invalid", false});
}
} // namespace

Result<MemoryState> reduce_memory_event(const MemoryState &current,
                                        const MemoryEvent &event) {
    const auto valid = validate_memory_event(event);
    if (!valid.has_value())
        return Result<MemoryState>::failure(valid.error());
    if (current.last_sequence == std::numeric_limits<std::uint64_t>::max() ||
        event.sequence != current.last_sequence + 1 ||
        (!current.last_timestamp_utc.empty() &&
         normalized_memory_timestamp(event.timestamp_utc) <
             normalized_memory_timestamp(current.last_timestamp_utc)))
        return invalid_transition();
    MemoryState next = current;
    const bool applied = std::visit(
        [&next](const auto &payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, MemoryUpsertedPayload>) {
                const auto &entry = payload.entry;
                const auto old = next.active_entries.find(entry.memory_id);
                if (old == next.active_entries.end()) {
                    if (next.used_memory_ids.count(entry.memory_id) != 0 ||
                        normalized_memory_timestamp(entry.created_at_utc) !=
                            normalized_memory_timestamp(entry.updated_at_utc))
                        return false;
                } else {
                    const auto &previous = old->second;
                    if (std::tie(previous.category, previous.scope_utf8,
                                 previous.source_session_id, previous.source_turn_start,
                                 previous.source_turn_end, previous.created_at_utc,
                                 previous.origin) !=
                            std::tie(entry.category, entry.scope_utf8,
                                     entry.source_session_id, entry.source_turn_start,
                                     entry.source_turn_end, entry.created_at_utc,
                                     entry.origin) ||
                        previous.content == entry.content ||
                        normalized_memory_timestamp(entry.updated_at_utc) <=
                            normalized_memory_timestamp(previous.updated_at_utc))
                        return false;
                }
                next.active_entries[entry.memory_id] = entry;
                next.used_memory_ids.insert(entry.memory_id);
                return true;
            } else if constexpr (std::is_same_v<T, MemoryForgottenPayload>) {
                return next.active_entries.erase(payload.memory_id) == 1;
            } else {
                const auto previous = next.session_checkpoints.find(payload.session_id);
                if (previous != next.session_checkpoints.end() &&
                    payload.through_turn <= previous->second)
                    return false;
                next.session_checkpoints[payload.session_id] = payload.through_turn;
                return true;
            }
        },
        event.payload);
    if (!applied)
        return invalid_transition();
    next.last_sequence = event.sequence;
    next.last_timestamp_utc = event.timestamp_utc;
    return Result<MemoryState>::success(std::move(next));
}

Result<MemoryState> replay_memory_events(const std::vector<MemoryEvent> &events) {
    MemoryState state;
    for (const auto &event : events) {
        auto next = reduce_memory_event(state, event);
        if (!next.has_value())
            return next;
        state = std::move(next.value());
    }
    return Result<MemoryState>::success(std::move(state));
}
} // namespace agent

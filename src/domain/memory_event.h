#pragma once

#include "domain/memory_state.h"
#include "domain/result.h"

#include <string_view>
#include <variant>

namespace agent {

enum class MemoryEventKind {
    MemoryUpserted,
    MemoryForgotten,
    SessionMemoryConsolidated
};
struct MemoryUpsertedPayload {
    MemoryEntry entry;
};
struct MemoryForgottenPayload {
    std::string memory_id;
};
struct SessionMemoryConsolidatedPayload {
    std::string session_id;
    std::uint64_t through_turn{0};
};
using MemoryEventPayload = std::variant<MemoryUpsertedPayload, MemoryForgottenPayload,
                                        SessionMemoryConsolidatedPayload>;
struct MemoryEvent {
    std::uint32_t schema_version{1};
    std::uint64_t sequence{0};
    std::string timestamp_utc;
    std::string correlation_id;
    MemoryEventPayload payload;
};

MemoryEventKind memory_event_kind(const MemoryEventPayload &payload);
bool is_valid_memory_id(std::string_view id) noexcept;
// Validate structure here; apply content policy in the application.
Result<void> validate_memory_event(const MemoryEvent &event);
// Validated timestamps use UTC seconds or three-digit milliseconds.
std::string normalized_memory_timestamp(const std::string &timestamp);

inline bool operator==(const MemoryUpsertedPayload &a, const MemoryUpsertedPayload &b) {
    return a.entry == b.entry;
}
inline bool operator==(const MemoryForgottenPayload &a,
                       const MemoryForgottenPayload &b) {
    return a.memory_id == b.memory_id;
}
inline bool operator==(const SessionMemoryConsolidatedPayload &a,
                       const SessionMemoryConsolidatedPayload &b) {
    return a.session_id == b.session_id && a.through_turn == b.through_turn;
}
inline bool operator==(const MemoryEvent &a, const MemoryEvent &b) {
    return std::tie(a.schema_version, a.sequence, a.timestamp_utc, a.correlation_id,
                    a.payload) == std::tie(b.schema_version, b.sequence,
                                           b.timestamp_utc, b.correlation_id,
                                           b.payload);
}

} // namespace agent

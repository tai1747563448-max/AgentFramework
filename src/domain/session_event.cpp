#include "domain/session_event.h"

#include <type_traits>
#include <variant>

namespace agent {

SessionEventKind session_event_kind(const SessionEventPayload& payload) {
    return std::visit(
        [](const auto& typed) {
            using Payload = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<Payload, SessionStartedPayload>) {
                return SessionEventKind::SessionStarted;
            } else if constexpr (
                std::is_same_v<Payload, SessionTurnStartedPayload>) {
                return SessionEventKind::TurnStarted;
            } else if constexpr (
                std::is_same_v<Payload, SessionTurnCommittedPayload>) {
                return SessionEventKind::TurnCommitted;
            } else if constexpr (
                std::is_same_v<Payload, SessionTurnFailedPayload>) {
                return SessionEventKind::TurnFailed;
            } else {
                return SessionEventKind::SessionCompacted;
            }
        },
        payload);
}

}  // namespace agent

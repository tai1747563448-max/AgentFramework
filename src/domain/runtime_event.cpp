#include "domain/runtime_event.h"

#include <type_traits>

namespace agent {
namespace {

template <typename>
struct AlwaysFalse : std::false_type {};

}  // namespace

EventKind event_kind(const EventPayload& payload) {
    return std::visit(
        [](const auto& event) -> EventKind {
            using Payload = std::decay_t<decltype(event)>;
            if constexpr (std::is_same_v<Payload, TaskStartedPayload>) {
                return EventKind::TaskStarted;
            } else if constexpr (std::is_same_v<Payload, ContextPreparationStartedPayload>) {
                return EventKind::ContextPreparationStarted;
            } else if constexpr (std::is_same_v<Payload, ContextPreparedPayload>) {
                return EventKind::ContextPrepared;
            } else if constexpr (std::is_same_v<Payload, KnowledgeNoMatchPayload>) {
                return EventKind::KnowledgeNoMatch;
            } else if constexpr (std::is_same_v<Payload, ContextPreparationFailedPayload>) {
                return EventKind::ContextPreparationFailed;
            } else if constexpr (std::is_same_v<Payload, ModelCallStartedPayload>) {
                return EventKind::ModelCallStarted;
            } else if constexpr (std::is_same_v<Payload, ModelCallSucceededPayload>) {
                return EventKind::ModelCallSucceeded;
            } else if constexpr (std::is_same_v<Payload, ModelCallFailedPayload>) {
                return EventKind::ModelCallFailed;
            } else if constexpr (std::is_same_v<Payload, ToolCallStartedPayload>) {
                return EventKind::ToolCallStarted;
            } else if constexpr (std::is_same_v<Payload, ToolCallSucceededPayload>) {
                return EventKind::ToolCallSucceeded;
            } else if constexpr (std::is_same_v<Payload, ToolCallFailedPayload>) {
                return EventKind::ToolCallFailed;
            } else if constexpr (std::is_same_v<Payload, TaskCompletedPayload>) {
                return EventKind::TaskCompleted;
            } else if constexpr (std::is_same_v<Payload, TaskFailedPayload>) {
                return EventKind::TaskFailed;
            } else if constexpr (std::is_same_v<Payload, TaskBudgetExceededPayload>) {
                return EventKind::TaskBudgetExceeded;
            } else if constexpr (std::is_same_v<Payload, TaskCancelledPayload>) {
                return EventKind::TaskCancelled;
            } else {
                static_assert(AlwaysFalse<Payload>::value, "unhandled event payload");
            }
        },
        payload);
}

}  // namespace agent

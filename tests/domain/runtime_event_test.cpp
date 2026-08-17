#include "domain/runtime_event.h"
#include "domain/task_state.h"
#include "test_support.h"

TEST_CASE(event_kind_is_derived_from_typed_payload) {
    agent::EventPayload payload = agent::ModelCallFailedPayload{
        {agent::ErrorCode::RequestTimeout, "model timeout", true}};
    REQUIRE(agent::event_kind(payload) == agent::EventKind::ModelCallFailed);
}

TEST_CASE(event_kind_maps_every_typed_payload) {
    const agent::RuntimeError error{
        agent::ErrorCode::TransportFailure, "transport failure", false};
    REQUIRE(agent::event_kind(agent::EventPayload{
                agent::TaskStartedPayload{"issue", "workspace", {}}}) ==
            agent::EventKind::TaskStarted);
    REQUIRE(agent::event_kind(agent::EventPayload{
                agent::ContextPreparationStartedPayload{}}) ==
            agent::EventKind::ContextPreparationStarted);
    REQUIRE(agent::event_kind(agent::EventPayload{
                agent::ContextPreparedPayload{}}) == agent::EventKind::ContextPrepared);
    REQUIRE(agent::event_kind(agent::EventPayload{
                agent::ContextPreparationFailedPayload{error}}) ==
            agent::EventKind::ContextPreparationFailed);
    REQUIRE(agent::event_kind(agent::EventPayload{
                agent::ModelCallStartedPayload{}}) == agent::EventKind::ModelCallStarted);
    REQUIRE(agent::event_kind(agent::EventPayload{
                agent::ModelCallSucceededPayload{}}) ==
            agent::EventKind::ModelCallSucceeded);
    REQUIRE(agent::event_kind(agent::EventPayload{
                agent::ModelCallFailedPayload{error}}) == agent::EventKind::ModelCallFailed);
    REQUIRE(agent::event_kind(agent::EventPayload{agent::ToolCallStartedPayload{
                {"call-1", "inspect", agent::Value::object({})}}}) ==
            agent::EventKind::ToolCallStarted);
    REQUIRE(agent::event_kind(agent::EventPayload{agent::ToolCallSucceededPayload{
                {"call-1", "contents", false}}}) == agent::EventKind::ToolCallSucceeded);
    REQUIRE(agent::event_kind(agent::EventPayload{
                agent::ToolCallFailedPayload{"call-1", error}}) ==
            agent::EventKind::ToolCallFailed);
    REQUIRE(agent::event_kind(agent::EventPayload{
                agent::TaskCompletedPayload{"done"}}) == agent::EventKind::TaskCompleted);
    REQUIRE(agent::event_kind(agent::EventPayload{
                agent::TaskFailedPayload{error}}) == agent::EventKind::TaskFailed);
    REQUIRE(agent::event_kind(agent::EventPayload{
                agent::TaskBudgetExceededPayload{"max_model_rounds", error}}) ==
            agent::EventKind::TaskBudgetExceeded);
    REQUIRE(agent::event_kind(agent::EventPayload{
                agent::TaskCancelledPayload{"cancelled", error}}) ==
            agent::EventKind::TaskCancelled);
}

TEST_CASE(only_four_task_statuses_are_terminal) {
    REQUIRE(agent::is_terminal(agent::TaskStatus::Completed));
    REQUIRE(agent::is_terminal(agent::TaskStatus::Failed));
    REQUIRE(agent::is_terminal(agent::TaskStatus::BudgetExceeded));
    REQUIRE(agent::is_terminal(agent::TaskStatus::Cancelled));
    REQUIRE(!agent::is_terminal(agent::TaskStatus::AwaitingModel));
}

#include "application/session_reducer.h"
#include "test_support.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fixtures {

constexpr const char* kSessionId =
    "session-11111111111111111111111111111111";
constexpr const char* kTaskOne =
    "task-11111111111111111111111111111111";
constexpr const char* kTaskTwo =
    "task-22222222222222222222222222222222";

agent::SessionEvent event(std::uint64_t sequence,
                          agent::SessionEventPayload payload) {
    return {1, sequence, kSessionId,
            "2026-09-06T12:00:00.000Z",
            "corr-session-" + std::to_string(sequence),
            std::move(payload)};
}

agent::SessionEvent started() {
    return event(1, agent::SessionStartedPayload{
                        u8"E:/工作区", "MiniMax-M3"});
}

std::vector<agent::Message> committed_messages(
    const std::string& user,
    const std::string& assistant) {
    return {
        {agent::Role::User, {agent::TextBlock{user}}},
        {agent::Role::Assistant, {agent::TextBlock{assistant}}},
    };
}

}  // namespace fixtures

TEST_CASE(session_reducer_replays_two_committed_turns_in_exact_order) {
    const auto first = fixtures::committed_messages("first", "answer one");
    const auto second = fixtures::committed_messages("second", "answer two");
    const std::vector<agent::SessionEvent> events{
        fixtures::started(),
        fixtures::event(2, agent::SessionTurnStartedPayload{
                               1, fixtures::kTaskOne, "first"}),
        fixtures::event(3, agent::SessionTurnCommittedPayload{
                               1, fixtures::kTaskOne, first}),
        fixtures::event(4, agent::SessionTurnStartedPayload{
                               2, fixtures::kTaskTwo, "second"}),
        fixtures::event(5, agent::SessionTurnCommittedPayload{
                               2, fixtures::kTaskTwo, second}),
    };

    const auto replayed = agent::replay_session_events(events);

    REQUIRE(replayed.has_value());
    REQUIRE(replayed.value().session_id == fixtures::kSessionId);
    REQUIRE(replayed.value().workspace_utf8 == u8"E:/工作区");
    REQUIRE(replayed.value().model == "MiniMax-M3");
    REQUIRE(replayed.value().completed_turns == 2);
    REQUIRE(!replayed.value().pending_turn.has_value());
    auto expected = first;
    expected.insert(expected.end(), second.begin(), second.end());
    REQUIRE(replayed.value().messages == expected);
    const std::vector<agent::CommittedSessionTurn> expected_turns{
        {1, fixtures::kTaskOne, first}, {2, fixtures::kTaskTwo, second}};
    REQUIRE(replayed.value().committed_turns == expected_turns);
    REQUIRE(replayed.value().summary.empty());
    REQUIRE(replayed.value().compacted_through_turn == 0);
    REQUIRE(replayed.value().last_task_id ==
            std::optional<std::string>{fixtures::kTaskTwo});
    REQUIRE(replayed.value().last_task_status ==
            std::optional<agent::TaskStatus>{agent::TaskStatus::Completed});
    REQUIRE(replayed.value().last_sequence == 5);
}

TEST_CASE(session_reducer_compacts_a_committed_prefix_and_retains_later_turns) {
    const auto first = fixtures::committed_messages("first", "answer one");
    const auto second = fixtures::committed_messages("second", "answer two");
    const auto third = fixtures::committed_messages("third", "answer three");
    const std::vector<agent::SessionEvent> events{
        fixtures::started(),
        fixtures::event(2, agent::SessionTurnStartedPayload{
                               1, fixtures::kTaskOne, "first"}),
        fixtures::event(3, agent::SessionTurnCommittedPayload{
                               1, fixtures::kTaskOne, first}),
        fixtures::event(4, agent::SessionTurnStartedPayload{
                               2, fixtures::kTaskTwo, "second"}),
        fixtures::event(5, agent::SessionTurnCommittedPayload{
                               2, fixtures::kTaskTwo, second}),
        fixtures::event(6, agent::SessionTurnStartedPayload{
                               3, fixtures::kTaskOne, "third"}),
        fixtures::event(7, agent::SessionTurnCommittedPayload{
                               3, fixtures::kTaskOne, third}),
        fixtures::event(8, agent::SessionCompactedPayload{
                               2, "The first two turns were resolved."}),
    };

    const auto replayed = agent::replay_session_events(events);

    REQUIRE(replayed.has_value());
    REQUIRE(replayed.value().summary ==
            "The first two turns were resolved.");
    REQUIRE(replayed.value().compacted_through_turn == 2);
    const std::vector<agent::CommittedSessionTurn> expected_turns{
        {3, fixtures::kTaskOne, third}};
    REQUIRE(replayed.value().committed_turns == expected_turns);
    REQUIRE(replayed.value().messages == third);
}

TEST_CASE(session_reducer_replaces_the_summary_when_compaction_is_cumulative) {
    const auto first = fixtures::committed_messages("first", "answer one");
    const auto second = fixtures::committed_messages("second", "answer two");
    const auto third = fixtures::committed_messages("third", "answer three");
    const auto fourth = fixtures::committed_messages("fourth", "answer four");
    const std::vector<agent::SessionEvent> events{
        fixtures::started(),
        fixtures::event(2, agent::SessionTurnStartedPayload{1, fixtures::kTaskOne, "first"}),
        fixtures::event(3, agent::SessionTurnCommittedPayload{1, fixtures::kTaskOne, first}),
        fixtures::event(4, agent::SessionTurnStartedPayload{2, fixtures::kTaskTwo, "second"}),
        fixtures::event(5, agent::SessionTurnCommittedPayload{2, fixtures::kTaskTwo, second}),
        fixtures::event(6, agent::SessionTurnStartedPayload{3, fixtures::kTaskOne, "third"}),
        fixtures::event(7, agent::SessionTurnCommittedPayload{3, fixtures::kTaskOne, third}),
        fixtures::event(8, agent::SessionCompactedPayload{2, "First summary."}),
        fixtures::event(9, agent::SessionTurnStartedPayload{4, fixtures::kTaskTwo, "fourth"}),
        fixtures::event(10, agent::SessionTurnCommittedPayload{4, fixtures::kTaskTwo, fourth}),
        fixtures::event(11, agent::SessionCompactedPayload{3, "Cumulative summary."}),
    };

    const auto replayed = agent::replay_session_events(events);

    REQUIRE(replayed.has_value());
    REQUIRE(replayed.value().summary == "Cumulative summary.");
    REQUIRE(replayed.value().compacted_through_turn == 3);
    const std::vector<agent::CommittedSessionTurn> expected_turns{
        {4, fixtures::kTaskTwo, fourth}};
    REQUIRE(replayed.value().committed_turns == expected_turns);
    REQUIRE(replayed.value().messages == fourth);
}

TEST_CASE(session_reducer_rejects_compaction_without_new_covered_turns_or_a_retained_turn) {
    const auto first = fixtures::committed_messages("first", "answer one");
    const auto second = fixtures::committed_messages("second", "answer two");
    auto state = agent::reduce_session_event(std::nullopt, fixtures::started());
    state = agent::reduce_session_event(
        state.value(), fixtures::event(2, agent::SessionTurnStartedPayload{1, fixtures::kTaskOne, "first"}));
    state = agent::reduce_session_event(
        state.value(), fixtures::event(3, agent::SessionTurnCommittedPayload{1, fixtures::kTaskOne, first}));
    state = agent::reduce_session_event(
        state.value(), fixtures::event(4, agent::SessionTurnStartedPayload{2, fixtures::kTaskTwo, "second"}));
    state = agent::reduce_session_event(
        state.value(), fixtures::event(5, agent::SessionTurnCommittedPayload{2, fixtures::kTaskTwo, second}));
    REQUIRE(state.has_value());

    const auto no_new_coverage = agent::reduce_session_event(
        state.value(), fixtures::event(6, agent::SessionCompactedPayload{0, "summary"}));
    REQUIRE(!no_new_coverage.has_value());
    REQUIRE(no_new_coverage.error().code == agent::ErrorCode::InvalidTransition);

    const auto removes_every_turn = agent::reduce_session_event(
        state.value(), fixtures::event(6, agent::SessionCompactedPayload{2, "summary"}));
    REQUIRE(!removes_every_turn.has_value());
    REQUIRE(removes_every_turn.error().code == agent::ErrorCode::InvalidTransition);
}

TEST_CASE(session_reducer_rejects_compaction_while_a_turn_is_pending) {
    const auto first = fixtures::committed_messages("first", "answer one");
    const auto second = fixtures::committed_messages("second", "answer two");
    auto state = agent::reduce_session_event(std::nullopt, fixtures::started());
    state = agent::reduce_session_event(
        state.value(), fixtures::event(2, agent::SessionTurnStartedPayload{1, fixtures::kTaskOne, "first"}));
    state = agent::reduce_session_event(
        state.value(), fixtures::event(3, agent::SessionTurnCommittedPayload{1, fixtures::kTaskOne, first}));
    state = agent::reduce_session_event(
        state.value(), fixtures::event(4, agent::SessionTurnStartedPayload{2, fixtures::kTaskTwo, "second"}));
    state = agent::reduce_session_event(
        state.value(), fixtures::event(5, agent::SessionTurnCommittedPayload{2, fixtures::kTaskTwo, second}));
    state = agent::reduce_session_event(
        state.value(), fixtures::event(6, agent::SessionTurnStartedPayload{3, fixtures::kTaskOne, "pending"}));
    REQUIRE(state.has_value());

    const auto rejected = agent::reduce_session_event(
        state.value(), fixtures::event(7, agent::SessionCompactedPayload{1, "summary"}));

    REQUIRE(!rejected.has_value());
    REQUIRE(rejected.error().code == agent::ErrorCode::InvalidTransition);
}

TEST_CASE(session_reducer_rejects_empty_or_oversized_compaction_summaries) {
    const auto first = fixtures::committed_messages("first", "answer one");
    const auto second = fixtures::committed_messages("second", "answer two");
    auto state = agent::reduce_session_event(std::nullopt, fixtures::started());
    state = agent::reduce_session_event(
        state.value(), fixtures::event(2, agent::SessionTurnStartedPayload{1, fixtures::kTaskOne, "first"}));
    state = agent::reduce_session_event(
        state.value(), fixtures::event(3, agent::SessionTurnCommittedPayload{1, fixtures::kTaskOne, first}));
    state = agent::reduce_session_event(
        state.value(), fixtures::event(4, agent::SessionTurnStartedPayload{2, fixtures::kTaskTwo, "second"}));
    state = agent::reduce_session_event(
        state.value(), fixtures::event(5, agent::SessionTurnCommittedPayload{2, fixtures::kTaskTwo, second}));
    REQUIRE(state.has_value());

    const auto empty = agent::reduce_session_event(
        state.value(), fixtures::event(6, agent::SessionCompactedPayload{1, ""}));
    REQUIRE(!empty.has_value());
    REQUIRE(empty.error().code == agent::ErrorCode::InvalidInput);

    const auto oversized = agent::reduce_session_event(
        state.value(), fixtures::event(6, agent::SessionCompactedPayload{
                               1, std::string(8193, 'x')}));
    REQUIRE(!oversized.has_value());
    REQUIRE(oversized.error().code == agent::ErrorCode::InvalidInput);
}

TEST_CASE(session_reducer_rejects_a_non_utf8_compaction_summary) {
    const auto first = fixtures::committed_messages("first", "answer one");
    const auto second = fixtures::committed_messages("second", "answer two");
    auto state = agent::reduce_session_event(std::nullopt, fixtures::started());
    state = agent::reduce_session_event(
        state.value(), fixtures::event(2, agent::SessionTurnStartedPayload{1, fixtures::kTaskOne, "first"}));
    state = agent::reduce_session_event(
        state.value(), fixtures::event(3, agent::SessionTurnCommittedPayload{1, fixtures::kTaskOne, first}));
    state = agent::reduce_session_event(
        state.value(), fixtures::event(4, agent::SessionTurnStartedPayload{2, fixtures::kTaskTwo, "second"}));
    state = agent::reduce_session_event(
        state.value(), fixtures::event(5, agent::SessionTurnCommittedPayload{2, fixtures::kTaskTwo, second}));
    REQUIRE(state.has_value());

    const auto rejected = agent::reduce_session_event(
        state.value(), fixtures::event(6, agent::SessionCompactedPayload{
                           1, std::string("\xC0\xAF")}));

    REQUIRE(!rejected.has_value());
    REQUIRE(rejected.error().code == agent::ErrorCode::InvalidInput);
}

TEST_CASE(session_reducer_keeps_failed_turn_out_of_active_context) {
    const std::vector<agent::SessionEvent> events{
        fixtures::started(),
        fixtures::event(2, agent::SessionTurnStartedPayload{
                               1, fixtures::kTaskOne, "will fail"}),
        fixtures::event(3, agent::SessionTurnFailedPayload{
                               1, fixtures::kTaskOne,
                               agent::TaskStatus::Failed,
                               "task failed"}),
    };

    const auto replayed = agent::replay_session_events(events);

    REQUIRE(replayed.has_value());
    REQUIRE(replayed.value().completed_turns == 1);
    REQUIRE(replayed.value().messages.empty());
    REQUIRE(replayed.value().last_task_status ==
            std::optional<agent::TaskStatus>{agent::TaskStatus::Failed});
}

TEST_CASE(session_reducer_rejects_a_second_pending_turn) {
    auto state = agent::reduce_session_event(std::nullopt, fixtures::started());
    REQUIRE(state.has_value());
    state = agent::reduce_session_event(
        state.value(),
        fixtures::event(2, agent::SessionTurnStartedPayload{
                               1, fixtures::kTaskOne, "first"}));
    REQUIRE(state.has_value());

    const auto rejected = agent::reduce_session_event(
        state.value(),
        fixtures::event(3, agent::SessionTurnStartedPayload{
                               2, fixtures::kTaskTwo, "second"}));

    REQUIRE(!rejected.has_value());
    REQUIRE(rejected.error().code == agent::ErrorCode::InvalidTransition);
}

TEST_CASE(session_reducer_rejects_commit_that_does_not_match_pending_turn) {
    auto state = agent::reduce_session_event(std::nullopt, fixtures::started());
    state = agent::reduce_session_event(
        state.value(),
        fixtures::event(2, agent::SessionTurnStartedPayload{
                               1, fixtures::kTaskOne, "first"}));
    REQUIRE(state.has_value());

    const auto rejected = agent::reduce_session_event(
        state.value(),
        fixtures::event(3, agent::SessionTurnCommittedPayload{
                               1, fixtures::kTaskTwo,
                               fixtures::committed_messages(
                                   "first", "answer")}));

    REQUIRE(!rejected.has_value());
    REQUIRE(rejected.error().code == agent::ErrorCode::InvalidTransition);
}

TEST_CASE(session_reducer_rejects_commit_with_wrong_user_message) {
    auto state = agent::reduce_session_event(std::nullopt, fixtures::started());
    state = agent::reduce_session_event(
        state.value(),
        fixtures::event(2, agent::SessionTurnStartedPayload{
                               1, fixtures::kTaskOne, "expected"}));
    REQUIRE(state.has_value());

    const auto rejected = agent::reduce_session_event(
        state.value(),
        fixtures::event(3, agent::SessionTurnCommittedPayload{
                               1, fixtures::kTaskOne,
                               fixtures::committed_messages(
                                   "different", "answer")}));

    REQUIRE(!rejected.has_value());
    REQUIRE(rejected.error().code == agent::ErrorCode::InvalidTransition);
}

TEST_CASE(session_reducer_rejects_noncontiguous_sequence_and_bad_session_id) {
    auto bad_sequence = fixtures::started();
    bad_sequence.sequence = 2;
    REQUIRE(!agent::reduce_session_event(std::nullopt, bad_sequence)
                 .has_value());

    auto bad_id = fixtures::started();
    bad_id.session_id = "session-invalid";
    const auto rejected =
        agent::reduce_session_event(std::nullopt, bad_id);
    REQUIRE(!rejected.has_value());
    REQUIRE(rejected.error().code == agent::ErrorCode::InvalidInput);
}

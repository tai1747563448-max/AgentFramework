#include "services/compact/model_stages.h"

#include "domain/result.h"
#include "domain/session_state.h"

#include <algorithm>

namespace agent::compact {

CompactOutcome AutoCompactSummary::evaluate(
    std::vector<Message>& messages,
    const CompactBudget& budget) {
    CompactOutcome outcome;
    outcome.bytes_before = measure_message_bytes(messages);
    if (budget.hard_limit_bytes == 0 ||
        budget.current_bytes < budget.hard_limit_bytes) {
        outcome.bytes_after = outcome.bytes_before;
        return outcome;
    }
    // Translate the in-memory messages into the session-turn
    // shape the existing ModelContextCompactor consumes. The
    // compactor was designed for the session engine; reusing it
    // here means T03 inherits its prompt, retries, and metrics
    // for free. CommittedSessionTurn carries std::vector<Message>
    // directly, so we wrap each input message into its own turn.
    std::vector<CommittedSessionTurn> turns;
    turns.reserve(messages.size());
    std::uint64_t turn_index = 0;
    for (const auto& m : messages) {
        CommittedSessionTurn turn;
        turn.turn_index = turn_index++;
        turn.messages.push_back(m);
        turns.push_back(std::move(turn));
    }
    ContextCompactionInput input;
    input.turns = std::move(turns);
    const auto summary_result = compactor_->compact(input);
    if (!summary_result.has_value()) {
        outcome.bytes_after = outcome.bytes_before;
        return outcome;
    }
    auto summary = summary_result.value();
    if (summary.size() > summary_byte_cap_) {
        summary.resize(summary_byte_cap_);
    }
    Message summary_message{Role::System, {TextBlock{std::move(summary)}}};
    std::vector<Message> next;
    next.reserve(messages.size() + 1);
    next.push_back(std::move(summary_message));
    // Keep at most retain_turns trailing turns so the model still
    // sees the most recent context after compaction.
    if (budget.retain_turns > 0 && messages.size() > budget.retain_turns) {
        for (std::size_t i = messages.size() - budget.retain_turns;
             i < messages.size(); ++i) {
            next.push_back(std::move(messages[i]));
        }
    } else {
        for (auto& m : messages) next.push_back(std::move(m));
    }
    messages = std::move(next);
    outcome.bytes_after = measure_message_bytes(messages);
    outcome.triggered = outcome.bytes_after < outcome.bytes_before;
    return outcome;
}

CompactOutcome ReactiveCompact::evaluate(
    std::vector<Message>& messages,
    const CompactBudget& budget) {
    CompactOutcome outcome;
    outcome.bytes_before = measure_message_bytes(messages);
    if (!triggered_) {
        // No reactive trigger fired since the last chain run; preserve
        // the no-op behaviour so the chain can wire this stage without
        // branching on source.
        outcome.bytes_after = outcome.bytes_before;
        return outcome;
    }
    triggered_ = false;
    if (budget.current_bytes <= budget.threshold_bytes) {
        // Already under threshold; nothing to do even when triggered.
        outcome.bytes_after = outcome.bytes_before;
        return outcome;
    }
    // Same wiring as AutoCompactSummary but driven by the trigger flag
    // rather than the hard_limit threshold.
    std::vector<CommittedSessionTurn> turns;
    turns.reserve(messages.size());
    std::uint64_t turn_index = 0;
    for (const auto& m : messages) {
        CommittedSessionTurn turn;
        turn.turn_index = turn_index++;
        turn.messages.push_back(m);
        turns.push_back(std::move(turn));
    }
    ContextCompactionInput input;
    input.turns = std::move(turns);
    const auto summary_result = compactor_->compact(input);
    if (!summary_result.has_value()) {
        outcome.bytes_after = outcome.bytes_before;
        return outcome;
    }
    auto summary = summary_result.value();
    if (summary.size() > summary_byte_cap_) {
        summary.resize(summary_byte_cap_);
    }
    Message summary_message{Role::System, {TextBlock{std::move(summary)}}};
    std::vector<Message> next;
    next.reserve(messages.size() + 1);
    next.push_back(std::move(summary_message));
    if (budget.retain_turns > 0 && messages.size() > budget.retain_turns) {
        for (std::size_t i = messages.size() - budget.retain_turns;
             i < messages.size(); ++i) {
            next.push_back(std::move(messages[i]));
        }
    } else {
        for (auto& m : messages) next.push_back(std::move(m));
    }
    messages = std::move(next);
    outcome.bytes_after = measure_message_bytes(messages);
    outcome.triggered = outcome.bytes_after < outcome.bytes_before;
    return outcome;
}

void ReactiveCompact::trigger() {
    triggered_ = true;
}

}  // namespace agent::compact

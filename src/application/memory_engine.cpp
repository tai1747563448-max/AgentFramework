#include "application/memory_engine.h"

#include "application/memory_policy.h"
#include "application/memory_reducer.h"
#include "application/memory_retriever.h"
#include "application/model_memory_support.h"
#include "application/session_reducer.h"
#include "ports/clock.h"
#include "ports/id_generator.h"
#include "ports/memory_store.h"
#include "ports/session_store.h"

#include <algorithm>
#include <set>
#include <utility>

namespace agent {
namespace {

RuntimeError persistence_error() {
    return {ErrorCode::PersistenceFailure, "memory persistence failed", true};
}

RuntimeError invalid_input() {
    return {ErrorCode::InvalidInput, "memory operation input is invalid", false};
}

bool valid_category(MemoryCategory category) {
    return category == MemoryCategory::Preference || category == MemoryCategory::Decision ||
           category == MemoryCategory::Fact || category == MemoryCategory::Workflow ||
           category == MemoryCategory::Constraint;
}

bool exact_duplicate(const MemoryState& state, const MemoryCandidate& candidate) {
    return std::any_of(state.active_entries.begin(), state.active_entries.end(),
        [&candidate](const auto& pair) {
            const auto& entry = pair.second;
            return entry.category == candidate.category && entry.scope_utf8 == candidate.scope_utf8 &&
                   entry.content == candidate.content;
        });
}

constexpr std::size_t kMaxConsolidationTranscriptBytes = 32 * 1024;
constexpr std::size_t kMaxConsolidationTurns = 32;

std::size_t serialized_turn_bytes(const CommittedSessionTurn& turn) {
    const auto serialized = model_memory_detail::transcript({turn});
    return serialized.at(0).dump().size();
}

} // namespace

MemoryEngine::MemoryEngine(MemoryStore& memories, SessionStore& sessions,
    MemoryConsolidator& consolidator, MemoryRetriever& retriever,
    MemoryPolicy& policy, Clock& clock, IdGenerator& ids)
    : memories_(memories), sessions_(sessions), consolidator_(consolidator),
      retriever_(retriever), policy_(policy), clock_(clock), ids_(ids) {}

Result<std::vector<MemoryEntry>> MemoryEngine::list(const std::string& workspace_utf8) const {
    try {
        const auto state = memories_.read_state();
        if (!state.has_value())
            return Result<std::vector<MemoryEntry>>::failure(persistence_error());
        std::vector<MemoryEntry> entries;
        for (const auto& pair : state.value().active_entries) {
            if (pair.second.scope_utf8.empty() || pair.second.scope_utf8 == workspace_utf8)
                entries.push_back(pair.second);
        }
        return Result<std::vector<MemoryEntry>>::success(std::move(entries));
    } catch (...) {
        return Result<std::vector<MemoryEntry>>::failure(persistence_error());
    }
}

Result<MemoryState> MemoryEngine::append_event(MemoryEventPayload payload) {
    // Reload the sequence before each append.
    const auto current = memories_.read_state();
    if (!current.has_value()) return Result<MemoryState>::failure(persistence_error());
    const auto timestamp = clock_.now_utc();
    if (auto* upsert = std::get_if<MemoryUpsertedPayload>(&payload)) {
        // Reject duplicate IDs, including forgotten entries.
        if (current.value().used_memory_ids.count(upsert->entry.memory_id) != 0)
            return Result<MemoryState>::failure(invalid_input());
        upsert->entry.created_at_utc = timestamp;
        upsert->entry.updated_at_utc = timestamp;
    }
    MemoryEvent event{1, current.value().last_sequence + 1, timestamp,
                      ids_.next_correlation_id(), std::move(payload)};
    auto reduced = reduce_memory_event(current.value(), event);
    if (!reduced.has_value()) return Result<MemoryState>::failure(invalid_input());
    const auto stored = memories_.append(event);
    if (!stored.has_value()) return Result<MemoryState>::failure(persistence_error());
    return reduced;
}

Result<MemoryEntry> MemoryEngine::append_entry(
    const MemoryCandidate& candidate, const std::string& session_id,
    std::uint64_t source_turn_start, std::uint64_t source_turn_end, MemoryOrigin origin) {
    if (!policy_.validate_candidate(candidate.content).has_value())
        return Result<MemoryEntry>::failure(invalid_input());
    const auto seed = ids_.next_task_id();
    if (!is_valid_task_id(seed)) return Result<MemoryEntry>::failure(invalid_input());
    const auto memory_id = "memory-" + seed.substr(5);
    auto appended = append_event(MemoryUpsertedPayload{
        {memory_id, candidate.category, candidate.scope_utf8, candidate.content,
         session_id, source_turn_start, source_turn_end, {}, {}, origin}});
    if (!appended.has_value()) return Result<MemoryEntry>::failure(appended.error());
    return Result<MemoryEntry>::success(appended.value().active_entries.at(memory_id));
}

Result<MemoryEntry> MemoryEngine::remember(
    const std::string& session_id, const std::string& content) {
    try {
        if (!is_valid_session_id(session_id) || !policy_.validate_candidate(content).has_value())
            return Result<MemoryEntry>::failure(invalid_input());
        const auto events = sessions_.read_session(session_id);
        if (!events.has_value()) return Result<MemoryEntry>::failure(persistence_error());
        const auto state = replay_session_events(events.value());
        if (!state.has_value() || state.value().session_id != session_id)
            return Result<MemoryEntry>::failure(persistence_error());
        // Empty sessions still require positive provenance.
        const auto turn = std::max<std::uint64_t>(1, state.value().completed_turns);
        return append_entry({MemoryCategory::Fact, state.value().workspace_utf8, content},
                            session_id, turn, turn, MemoryOrigin::ExplicitUser);
    } catch (...) {
        return Result<MemoryEntry>::failure(persistence_error());
    }
}

Result<void> MemoryEngine::forget(const std::string& memory_id) {
    try {
        if (!is_valid_memory_id(memory_id)) return Result<void>::failure(invalid_input());
        const auto appended = append_event(MemoryForgottenPayload{memory_id});
        if (!appended.has_value()) return Result<void>::failure(appended.error());
        return Result<void>::success();
    } catch (...) {
        return Result<void>::failure(persistence_error());
    }
}

Result<std::vector<std::string>> MemoryEngine::retrieve(
    const std::string& workspace_utf8, const std::string& query,
    std::size_t top_k, std::size_t injected_byte_budget) const {
    try {
        const auto state = memories_.read_state();
        if (!state.has_value())
            return Result<std::vector<std::string>>::failure(persistence_error());
        auto result = retriever_.retrieve(state.value(), workspace_utf8, query,
                                          top_k, injected_byte_budget);
        if (!result.has_value())
            return Result<std::vector<std::string>>::failure(invalid_input());
        return result;
    } catch (...) {
        return Result<std::vector<std::string>>::failure(persistence_error());
    }
}

Result<MemoryEngine::MaintenanceTranscript>
MemoryEngine::read_maintenance_transcript(const std::string& session_id,
                                          std::uint64_t through_sequence) const {
    try {
        if (!is_valid_session_id(session_id))
            return Result<MaintenanceTranscript>::failure(invalid_input());
        const auto memory = memories_.read_state();
        const auto events = sessions_.read_session(session_id);
        if (!memory.has_value() || !events.has_value())
            return Result<MaintenanceTranscript>::failure(persistence_error());
        const auto session = replay_session_events(events.value());
        if (!session.has_value() || session.value().session_id != session_id)
            return Result<MaintenanceTranscript>::failure(persistence_error());
        const auto checkpoint_entry =
            memory.value().session_checkpoints.find(session_id);
        const auto checkpoint = checkpoint_entry == memory.value().session_checkpoints.end()
                                    ? 0ULL
                                    : checkpoint_entry->second;
        const auto through = std::min<std::uint64_t>(
            session.value().completed_turns, through_sequence);
        if (checkpoint > through)
            return Result<MaintenanceTranscript>::failure(persistence_error());
        MaintenanceTranscript result;
        result.workspace_utf8 = session.value().workspace_utf8;
        result.checkpoint = checkpoint;
        result.through_turn = through;
        for (const auto& event : events.value()) {
            const auto* committed =
                std::get_if<SessionTurnCommittedPayload>(&event.payload);
            if (committed != nullptr && committed->turn_index > checkpoint &&
                committed->turn_index <= through) {
                result.committed_turns.push_back(
                    {committed->turn_index, committed->task_id,
                     committed->messages});
            }
        }
        std::sort(result.committed_turns.begin(), result.committed_turns.end(),
                  [](const CommittedSessionTurn& left,
                     const CommittedSessionTurn& right) {
                      return left.turn_index < right.turn_index;
                  });
        return Result<MaintenanceTranscript>::success(std::move(result));
    } catch (...) {
        return Result<MaintenanceTranscript>::failure(persistence_error());
    }
}

std::vector<MemoryEngine::MaintenanceBatch>
MemoryEngine::build_batches(const MaintenanceTranscript& transcript) const {
    std::vector<MaintenanceBatch> batches;
    std::uint64_t cursor = transcript.checkpoint;
    std::size_t next_turn = 0;
    while (cursor < transcript.through_turn) {
        while (next_turn < transcript.committed_turns.size() &&
               transcript.committed_turns[next_turn].turn_index <= cursor)
            ++next_turn;

        if (next_turn == transcript.committed_turns.size()) {
            MaintenanceBatch batch;
            batch.source_turn_start = cursor + 1;
            batch.source_turn_end = transcript.through_turn;
            batches.push_back(std::move(batch));
            cursor = transcript.through_turn;
            continue;
        }

        const auto first_bytes =
            serialized_turn_bytes(transcript.committed_turns[next_turn]);
        if (first_bytes > kMaxConsolidationTranscriptBytes - 2) {
            cursor = transcript.committed_turns[next_turn].turn_index;
            ++next_turn;
            MaintenanceBatch batch;
            batch.source_turn_start = cursor;
            batch.source_turn_end = cursor;
            batches.push_back(std::move(batch));
            continue;
        }

        MaintenanceBatch batch;
        batch.source_turn_start = cursor + 1;
        std::size_t transcript_bytes = 2;
        while (next_turn < transcript.committed_turns.size() &&
               batch.turns.size() < kMaxConsolidationTurns) {
            const auto turn_bytes =
                serialized_turn_bytes(transcript.committed_turns[next_turn]);
            const auto separator = batch.turns.empty() ? 0U : 1U;
            if (transcript_bytes >
                    kMaxConsolidationTranscriptBytes - separator ||
                turn_bytes > kMaxConsolidationTranscriptBytes -
                                 transcript_bytes - separator)
                break;
            transcript_bytes += separator + turn_bytes;
            batch.turns.push_back(transcript.committed_turns[next_turn]);
            ++next_turn;
        }
        if (batch.turns.empty()) {
            MaintenanceBatch padding;
            padding.source_turn_start = cursor + 1;
            padding.source_turn_end = cursor;
            batches.push_back(std::move(padding));
            break;
        }
        batch.source_turn_end =
            next_turn < transcript.committed_turns.size()
                ? transcript.committed_turns[next_turn].turn_index - 1
                : transcript.through_turn;
        cursor = batch.source_turn_end;
        batches.push_back(std::move(batch));
    }
    return batches;
}

Result<std::vector<MemoryCandidate>> MemoryEngine::extract_candidates(
    const std::string& session_id, const MaintenanceTranscript& transcript,
    const MaintenanceBatch& batch,
    const std::atomic<bool>* cancelled) const {
    try {
        if (batch.turns.empty())
            return Result<std::vector<MemoryCandidate>>::success({});
        if (cancelled != nullptr && cancelled->load())
            return Result<std::vector<MemoryCandidate>>::failure(
                {ErrorCode::Cancelled, "memory maintenance pre-empted", false});
        MemoryConsolidationInput input{
            session_id, transcript.workspace_utf8,
            batch.source_turn_start, batch.source_turn_end,
            batch.turns, 8};
        auto candidates = consolidator_.consolidate(input);
        if (!candidates.has_value())
            return Result<std::vector<MemoryCandidate>>::failure(
                {candidates.error().code, "memory consolidation failed",
                 candidates.error().retryable});
        if (candidates.value().size() > input.max_candidates)
            return Result<std::vector<MemoryCandidate>>::failure(
                {ErrorCode::ProtocolFailure,
                 "memory consolidation result is invalid", false});
        if (cancelled != nullptr && cancelled->load())
            return Result<std::vector<MemoryCandidate>>::failure(
                {ErrorCode::Cancelled, "memory maintenance pre-empted", false});
        return candidates;
    } catch (...) {
        return Result<std::vector<MemoryCandidate>>::failure(
            {ErrorCode::ProtocolFailure, "memory consolidation failed", true});
    }
}

Result<MemoryEngine::CommitOutcome> MemoryEngine::commit_single_owner(
    const std::string& session_id, const std::vector<MemoryCandidate>& candidates,
    std::uint64_t source_turn_start, std::uint64_t source_turn_end,
    const std::vector<std::string>& invalidated_memory_ids) {
    CommitOutcome outcome;
    outcome.committed_through_turn = source_turn_end;
    if (!is_valid_session_id(session_id))
        return Result<CommitOutcome>::failure(invalid_input());
    const auto memory = memories_.read_state();
    if (!memory.has_value())
        return Result<CommitOutcome>::failure(persistence_error());
    const auto checkpoint_entry =
        memory.value().session_checkpoints.find(session_id);
    const auto checkpoint = checkpoint_entry == memory.value().session_checkpoints.end()
                                ? 0ULL
                                : checkpoint_entry->second;
    if (source_turn_end <= checkpoint) {
        return Result<CommitOutcome>::success(outcome);
    }
    std::string workspace_utf8;
    {
        const auto events = sessions_.read_session(session_id);
        if (events.has_value()) {
            const auto replayed = replay_session_events(events.value());
            if (replayed.has_value() &&
                replayed.value().session_id == session_id) {
                workspace_utf8 = replayed.value().workspace_utf8;
            }
        }
    }
    std::set<std::string> forgotten(invalidated_memory_ids.begin(),
                                     invalidated_memory_ids.end());
    for (const auto& candidate : candidates) {
        const auto safe = policy_.validate_candidate(candidate.content);
        if (!safe.has_value() || !valid_category(candidate.category) ||
            (!candidate.scope_utf8.empty() &&
             !workspace_utf8.empty() &&
             candidate.scope_utf8 != workspace_utf8))
            continue;
        const auto latest = memories_.read_state();
        if (!latest.has_value())
            return Result<CommitOutcome>::failure(persistence_error());
        const auto entry_id = "memory-pending-" + candidate.content;
        if (forgotten.count(entry_id) != 0 ||
            forgotten.count(candidate.content) != 0) {
            outcome.rejected_memory_ids.push_back(entry_id);
            continue;
        }
        if (exact_duplicate(latest.value(), candidate)) continue;
        const auto entry =
            append_entry(candidate, session_id, source_turn_start,
                         source_turn_end, MemoryOrigin::ModelConsolidation);
        if (!entry.has_value())
            return Result<CommitOutcome>::failure(entry.error());
        ++outcome.appended_entries;
    }
    const auto checkpointed = append_event(
        SessionMemoryConsolidatedPayload{session_id, source_turn_end});
    if (!checkpointed.has_value())
        return Result<CommitOutcome>::failure(checkpointed.error());
    outcome.checkpoint_advanced = true;
    return Result<CommitOutcome>::success(outcome);
}

Result<MemoryEngine::CommitOutcome> MemoryEngine::commit_maintenance(
    const std::string& session_id, const std::vector<MemoryCandidate>& candidates,
    const MaintenanceBatch& batch,
    const std::vector<std::string>& invalidated_memory_ids) {
    try {
        return commit_single_owner(session_id, candidates,
                                   batch.source_turn_start,
                                   batch.source_turn_end,
                                   invalidated_memory_ids);
    } catch (...) {
        return Result<CommitOutcome>::failure(persistence_error());
    }
}

Result<MemoryConsolidationResult> MemoryEngine::consolidate(const std::string& session_id) {
    try {
        if (!is_valid_session_id(session_id))
            return Result<MemoryConsolidationResult>::failure(invalid_input());
        const auto memory = memories_.read_state();
        const auto events = sessions_.read_session(session_id);
        if (!memory.has_value() || !events.has_value())
            return Result<MemoryConsolidationResult>::failure(persistence_error());
        const auto session = replay_session_events(events.value());
        if (!session.has_value() || session.value().session_id != session_id)
            return Result<MemoryConsolidationResult>::failure(persistence_error());
        const auto checkpoint = memory.value().session_checkpoints.find(session_id);
        const auto previous = checkpoint == memory.value().session_checkpoints.end() ? 0 : checkpoint->second;
        const auto through = session.value().completed_turns;
        if (previous > through)
            return Result<MemoryConsolidationResult>::failure(persistence_error());
        MemoryConsolidationResult result{0, through, false};
        if (previous == through)
            return Result<MemoryConsolidationResult>::success(result);

        // Read exact committed turns from the validated event log.
        std::vector<CommittedSessionTurn> committed_turns;
        for (const auto& event : events.value()) {
            const auto* committed = std::get_if<SessionTurnCommittedPayload>(&event.payload);
            if (committed != nullptr && committed->turn_index > previous && committed->turn_index <= through)
                committed_turns.push_back(
                    {committed->turn_index, committed->task_id, committed->messages});
        }

        std::uint64_t cursor = previous;
        std::size_t next_turn = 0;
        while (cursor < through) {
            while (next_turn < committed_turns.size() &&
                   committed_turns[next_turn].turn_index <= cursor)
                ++next_turn;

            if (next_turn == committed_turns.size()) {
                const auto checkpointed = append_event(
                    SessionMemoryConsolidatedPayload{session_id, through});
                if (!checkpointed.has_value())
                    return Result<MemoryConsolidationResult>::failure(
                        checkpointed.error());
                cursor = through;
                continue;
            }

            const auto first_bytes =
                serialized_turn_bytes(committed_turns[next_turn]);
            if (first_bytes > kMaxConsolidationTranscriptBytes - 2) {
                // Keep oversized turns in JSONL; checkpoint without extraction.
                cursor = committed_turns[next_turn].turn_index;
                ++next_turn;
                const auto checkpointed = append_event(
                    SessionMemoryConsolidatedPayload{session_id, cursor});
                if (!checkpointed.has_value())
                    return Result<MemoryConsolidationResult>::failure(
                        checkpointed.error());
                continue;
            }

            MemoryConsolidationInput input{
                session_id, session.value().workspace_utf8,
                cursor + 1, cursor, {}, 8};
            std::size_t transcript_bytes = 2;
            while (next_turn < committed_turns.size() &&
                   input.turns.size() < kMaxConsolidationTurns) {
                const auto turn_bytes =
                    serialized_turn_bytes(committed_turns[next_turn]);
                const auto separator = input.turns.empty() ? 0U : 1U;
                if (transcript_bytes >
                        kMaxConsolidationTranscriptBytes - separator ||
                    turn_bytes > kMaxConsolidationTranscriptBytes -
                                     transcript_bytes - separator)
                    break;
                transcript_bytes += separator + turn_bytes;
                input.turns.push_back(committed_turns[next_turn]);
                ++next_turn;
            }
            if (input.turns.empty())
                return Result<MemoryConsolidationResult>::failure(
                    {ErrorCode::InvalidConfiguration,
                     "memory consolidation batch is invalid", false});

            input.source_turn_end =
                next_turn < committed_turns.size()
                    ? committed_turns[next_turn].turn_index - 1
                    : through;
            const auto candidates = [&]() {
                try {
                    return consolidator_.consolidate(input);
                } catch (...) {
                    return Result<std::vector<MemoryCandidate>>::failure(
                        {ErrorCode::ProtocolFailure, "memory consolidation failed", true});
                }
            }();
            if (!candidates.has_value())
                return Result<MemoryConsolidationResult>::failure(
                    {candidates.error().code, "memory consolidation failed", candidates.error().retryable});
            if (candidates.value().size() > input.max_candidates)
                return Result<MemoryConsolidationResult>::failure(
                    {ErrorCode::ProtocolFailure, "memory consolidation result is invalid", false});
            result.model_called = true;
            for (const auto& candidate : candidates.value()) {
                const auto safe = policy_.validate_candidate(candidate.content);
                if (!safe.has_value() || !valid_category(candidate.category) ||
                    (!candidate.scope_utf8.empty() && candidate.scope_utf8 != input.workspace_utf8))
                    continue;
                const auto latest = memories_.read_state();
                if (!latest.has_value())
                    return Result<MemoryConsolidationResult>::failure(persistence_error());
                if (exact_duplicate(latest.value(), candidate)) continue;
                const auto entry = append_entry(candidate, session_id, input.source_turn_start,
                                                 input.source_turn_end,
                                                 MemoryOrigin::ModelConsolidation);
                if (!entry.has_value())
                    return Result<MemoryConsolidationResult>::failure(entry.error());
                ++result.appended_entries;
            }
            const auto checkpointed = append_event(
                SessionMemoryConsolidatedPayload{session_id,
                                                 input.source_turn_end});
            if (!checkpointed.has_value())
                return Result<MemoryConsolidationResult>::failure(
                    checkpointed.error());
            cursor = input.source_turn_end;
        }
        return Result<MemoryConsolidationResult>::success(result);
    } catch (...) {
        return Result<MemoryConsolidationResult>::failure(persistence_error());
    }
}

} // namespace agent

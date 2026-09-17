#pragma once

#include "domain/memory_event.h"
#include "ports/memory_consolidator.h"

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

namespace agent {

class Clock;
class IdGenerator;
class MemoryStore;
class SessionStore;
class MemoryRetriever;
class MemoryPolicy;

struct MemoryConsolidationResult {
    std::size_t appended_entries{0};
    std::uint64_t through_turn{0};
    bool model_called{false};
};

class MemoryEngine {
public:
    MemoryEngine(MemoryStore& memories, SessionStore& sessions,
                 MemoryConsolidator& consolidator, MemoryRetriever& retriever,
                 MemoryPolicy& policy, Clock& clock, IdGenerator& ids);

    Result<std::vector<MemoryEntry>> list(const std::string& workspace_utf8) const;
    Result<MemoryEntry> remember(const std::string& session_id,
                                 const std::string& content);
    Result<void> forget(const std::string& memory_id);
    Result<std::vector<std::string>> retrieve(const std::string& workspace_utf8,
                                             const std::string& query,
                                             std::size_t top_k,
                                             std::size_t injected_byte_budget) const;
    Result<MemoryConsolidationResult> consolidate(const std::string& session_id);

    // Three-step maintenance API used by MemoryMaintenanceScheduler. Each step
    // is independently callable so the scheduler can split read/extract/commit
    // across background workers and respect the single-owner commit rule.
    //
    // Step 1: read the committed transcript up to `through_sequence` and the
    // current checkpoint. The transcript is read directly from the persisted
    // event log so it reflects committed events even when foreground turns
    // have not yet asked for a snapshot.
    struct MaintenanceTranscript {
        std::string workspace_utf8;
        std::uint64_t checkpoint{0};
        std::uint64_t through_turn{0};
        std::vector<CommittedSessionTurn> committed_turns;
    };
    Result<MaintenanceTranscript> read_maintenance_transcript(
        const std::string& session_id, std::uint64_t through_sequence) const;

    // Step 2: build a single batch input and call the extraction client. The
    // caller owns the candidate vector and can decide whether to commit,
    // discard, or split the batch. `cancelled` is checked between turns; the
    // scheduler can flip it from another thread to abort a long extraction.
    struct MaintenanceBatch {
        std::uint64_t source_turn_start{0};
        std::uint64_t source_turn_end{0};
        std::vector<CommittedSessionTurn> turns;
    };
    Result<std::vector<MemoryCandidate>> extract_candidates(
        const std::string& session_id, const MaintenanceTranscript& transcript,
        const MaintenanceBatch& batch,
        const std::atomic<bool>* cancelled) const;

    // Step 3: commit a batch of candidates from the given source turn range.
    // Returns the number of entries actually appended. Performs a version
    // check against the persisted checkpoint and refuses to advance the
    // checkpoint past the requested `source_turn_end`. If a `forget` event
    // arrived for any of the candidate ids, the corresponding entries are
    // not appended.
    struct CommitOutcome {
        std::uint64_t appended_entries{0};
        std::uint64_t committed_through_turn{0};
        std::vector<std::string> rejected_memory_ids;
        bool checkpoint_advanced{false};
    };
    Result<CommitOutcome> commit_maintenance(
        const std::string& session_id, const std::vector<MemoryCandidate>& candidates,
        const MaintenanceBatch& batch,
        const std::vector<std::string>& invalidated_memory_ids);

    // Build batches from a transcript using the documented turn/byte budgets.
    // Public so the maintenance scheduler can produce the same batch layout
    // the synchronous consolidate() uses.
    std::vector<MaintenanceBatch> build_batches(
        const MaintenanceTranscript& transcript) const;

private:
    Result<MemoryState> append_event(MemoryEventPayload payload);
    Result<MemoryEntry> append_entry(const MemoryCandidate& candidate,
                                     const std::string& session_id,
                                     std::uint64_t source_turn_start,
                                     std::uint64_t source_turn_end,
                                     MemoryOrigin origin);
    // Single-owner commit: re-reads memory state and applies exactly one
    // checkpoint per call. Used by consolidate() and commit_maintenance().
    Result<CommitOutcome> commit_single_owner(
        const std::string& session_id, const std::vector<MemoryCandidate>& candidates,
        std::uint64_t source_turn_start, std::uint64_t source_turn_end,
        const std::vector<std::string>& invalidated_memory_ids);

    MemoryStore& memories_;
    SessionStore& sessions_;
    MemoryConsolidator& consolidator_;
    MemoryRetriever& retriever_;
    MemoryPolicy& policy_;
    Clock& clock_;
    IdGenerator& ids_;
};

} // namespace agent

#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>

namespace agent {

enum class MemoryCategory { Preference, Decision, Fact, Workflow, Constraint };
enum class MemoryOrigin { ExplicitUser, ModelConsolidation };

struct MemoryEntry {
    std::string memory_id;
    MemoryCategory category{MemoryCategory::Fact};
    std::string scope_utf8;
    std::string content;
    std::string source_session_id;
    std::uint64_t source_turn_start{0};
    std::uint64_t source_turn_end{0};
    std::string created_at_utc;
    std::string updated_at_utc;
    MemoryOrigin origin{MemoryOrigin::ExplicitUser};
    // Stage 8 (and future stages): cross-references that aren't part
    // of the JSONL event stream today. They live in memory state but
    // are populated by orchestration code, not the reducer.
    std::string source_period_id;
    // Stage 6: retrieval-coefficient normalisation fields. Not part of
    // the JSONL event stream yet; they're derived at runtime from
    // access patterns. Future stages may persist them.
    double retrieval_coefficient{0.0};
    std::uint32_t access_count{0};
    std::size_t curate_boost_remaining{0};
    double retrieval_difficulty{0.5};
};

inline bool operator==(const MemoryEntry &a, const MemoryEntry &b) {
    return std::tie(a.memory_id, a.category, a.scope_utf8, a.content,
                    a.source_session_id, a.source_turn_start, a.source_turn_end,
                    a.created_at_utc, a.updated_at_utc, a.origin,
                    a.source_period_id, a.retrieval_coefficient, a.access_count,
                    a.curate_boost_remaining, a.retrieval_difficulty) ==
           std::tie(b.memory_id, b.category, b.scope_utf8, b.content,
                    b.source_session_id, b.source_turn_start, b.source_turn_end,
                    b.created_at_utc, b.updated_at_utc, b.origin,
                    b.source_period_id, b.retrieval_coefficient, b.access_count,
                    b.curate_boost_remaining, b.retrieval_difficulty);
}

struct MemoryState {
    std::uint64_t last_sequence{0};
    std::string last_timestamp_utc;
    std::map<std::string, MemoryEntry> active_entries;
    std::map<std::string, std::uint64_t> session_checkpoints;
    // Keep forgotten IDs to prevent reuse.
    std::set<std::string> used_memory_ids;
    // T1 (stage 1): time/throughput bookkeeping for the periodic consolidation
    // scheduler. Replay-computed from the event stream; empty/zero means
    // "never consolidated", which intentionally short-circuits the throttle.
    std::string last_consolidated_at_utc;
    std::size_t tokens_since_last_consolidation{0};
};

inline bool operator==(const MemoryState &a, const MemoryState &b) {
    return std::tie(a.last_sequence, a.last_timestamp_utc, a.active_entries,
                    a.session_checkpoints, a.used_memory_ids,
                    a.last_consolidated_at_utc,
                    a.tokens_since_last_consolidation) ==
           std::tie(b.last_sequence, b.last_timestamp_utc, b.active_entries,
                    b.session_checkpoints, b.used_memory_ids,
                    b.last_consolidated_at_utc,
                    b.tokens_since_last_consolidation);
}

} // namespace agent

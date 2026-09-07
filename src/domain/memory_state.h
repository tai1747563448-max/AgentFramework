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
};

inline bool operator==(const MemoryEntry &a, const MemoryEntry &b) {
    return std::tie(a.memory_id, a.category, a.scope_utf8, a.content,
                    a.source_session_id, a.source_turn_start, a.source_turn_end,
                    a.created_at_utc, a.updated_at_utc, a.origin) ==
           std::tie(b.memory_id, b.category, b.scope_utf8, b.content,
                    b.source_session_id, b.source_turn_start, b.source_turn_end,
                    b.created_at_utc, b.updated_at_utc, b.origin);
}

struct MemoryState {
    std::uint64_t last_sequence{0};
    std::string last_timestamp_utc;
    std::map<std::string, MemoryEntry> active_entries;
    std::map<std::string, std::uint64_t> session_checkpoints;
    // Retain identities after forgetting, so an old id cannot be resurrected.
    std::set<std::string> used_memory_ids;
};

inline bool operator==(const MemoryState &a, const MemoryState &b) {
    return std::tie(a.last_sequence, a.last_timestamp_utc, a.active_entries,
                    a.session_checkpoints, a.used_memory_ids) ==
           std::tie(b.last_sequence, b.last_timestamp_utc, b.active_entries,
                    b.session_checkpoints, b.used_memory_ids);
}

} // namespace agent

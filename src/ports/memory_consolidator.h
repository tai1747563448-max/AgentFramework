#pragma once

#include "domain/memory_state.h"
#include "domain/result.h"
#include "domain/session_state.h"

#include <cstddef>

namespace agent {

struct MemoryConsolidationInput {
    std::string session_id;
    std::string workspace_utf8;
    std::uint64_t source_turn_start{0};
    std::uint64_t source_turn_end{0};
    std::vector<CommittedSessionTurn> turns;
    std::size_t max_candidates{8};
};

// No identity, timestamps, or provenance: the caller assigns these only after
// policy validation. Empty scope means global; otherwise it is the workspace.
struct MemoryCandidate {
    MemoryCategory category{MemoryCategory::Fact};
    std::string scope_utf8;
    std::string content;
};

class MemoryConsolidator {
public:
    virtual ~MemoryConsolidator() = default;
    virtual Result<std::vector<MemoryCandidate>> consolidate(
        const MemoryConsolidationInput& input) = 0;
};

} // namespace agent

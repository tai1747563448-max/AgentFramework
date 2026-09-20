#pragma once

#include "domain/memory_state.h"
#include "domain/period_state.h"
#include "domain/result.h"

#include <string>
#include <vector>

namespace agent {

// Stage 5b: REM-style refinement pass. Pure data structure + scheduler
// hooks; the actual LLM-driven extraction lives behind a seam so tests
// can substitute a deterministic extractor. The pass runs after every
// scheduled consolidation commit and produces a RefinementReport.
//
// Stage 5b ships the Abstract + Deduced passes as local algorithms
// (no LLM calls yet — those land when the agent framework's LLM
// seam is wired for consolidate). Conflict / supersede / cluster
// detection are deferred to a follow-up stage since they require an
// external model.
struct RefinementConfig {
    bool enabled{true};
    double abstract_threshold{0.7};
    std::size_t max_llm_calls_per_period{50};
    bool degrade_on_budget_exceeded{true};
};

struct RefinementReport {
    std::size_t abstracted{0};
    std::size_t deduced{0};
    std::size_t llm_calls{0};
    bool degraded{false};
};

class RefinementPass {
public:
    RefinementPass(RefinementConfig config = {});

    RefinementReport run(const MemoryState& state,
                         const std::vector<MemoryEntry>& newly_committed);

    static std::vector<std::string> find_deduced_facts(
        const std::vector<MemoryEntry>& grounded,
        std::size_t budget);

private:
    RefinementConfig config_;
};

}  // namespace agent

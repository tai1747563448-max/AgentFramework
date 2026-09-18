#pragma once

#include "application/model_context_compactor.h"
#include "ports/context_compactor.h"
#include "services/compact/compact_chain.h"

#include <memory>

namespace agent::compact {

// AutoCompactSummary wraps the existing ModelContextCompactor.
// Its trigger fires when budget.hard_limit_bytes is exceeded;
// the stage calls the compactor and replaces the session turns
// with the produced summary (kept as the first committed turn).
// The stage is intentionally cheap to skip: the chain stops
// when current_bytes drops below threshold_bytes so the LLM
// only runs in the worst case.
class AutoCompactSummary final : public CompactStage {
public:
    AutoCompactSummary(ContextCompactor& compactor,
                        std::size_t summary_byte_cap)
        : compactor_(&compactor), summary_byte_cap_(summary_byte_cap) {}
    std::string name() const override { return "auto_compact_summary"; }
    CompactOutcome evaluate(
        std::vector<Message>& messages,
        const CompactBudget& budget) override;

private:
    ContextCompactor* compactor_;
    std::size_t summary_byte_cap_;
};

// ReactiveCompact is a no-op stage reserved for T25 (reactive
// triggers). Calling evaluate is a no-op so the chain can wire
// it without branching on source. A future commit will replace
// the body with the actual Ctrl+C long-press / hook handling.
class ReactiveCompact final : public CompactStage {
public:
    std::string name() const override { return "reactive_compact"; }
    CompactOutcome evaluate(
        std::vector<Message>& /*messages*/,
        const CompactBudget& /*budget*/) override;
};

}  // namespace agent::compact

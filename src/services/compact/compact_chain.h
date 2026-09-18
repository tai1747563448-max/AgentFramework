#pragma once

#include "domain/latency_trace.h"
#include "domain/model_types.h"
#include "domain/session_state.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace agent::compact {

// CompactBudget is the snapshot of remaining capacity the chain
// uses to decide whether to keep running stages. The values come
// from SessionContextSettings and RuntimeBudgets; both are inlined
// here so the chain stays decoupled from the session engine.
struct CompactBudget {
    std::size_t threshold_bytes{0};
    std::size_t hard_limit_bytes{0};
    std::size_t current_bytes{0};
    std::size_t retain_turns{0};
};

// CompactOutcome is what each stage returns. triggered=true means
// the stage mutated messages; bytes_freed counts the size delta
// (positive when bytes dropped). stages report the same metric
// shape so the chain can sort or rank without inspecting stage
// internals.
struct CompactOutcome {
    bool triggered{false};
    std::size_t bytes_before{0};
    std::size_t bytes_after{0};
    std::int64_t elapsed_us{0};
};

inline std::size_t bytes_freed(const CompactOutcome& outcome) {
    if (outcome.bytes_after >= outcome.bytes_before) return 0;
    return outcome.bytes_before - outcome.bytes_after;
}

// CompactStage is the strategy interface. Each stage is short-
// lived: it inspects the current messages, may mutate them in
// place, and reports what it did. Stages that need an LLM (the two
// summarisers) hold a model reference injected at construction.
class CompactStage {
public:
    virtual ~CompactStage() = default;
    virtual std::string name() const = 0;
    // evaluate mutates messages in place when its trigger fires.
    // Returns the outcome (triggered flag + size deltas). The chain
    // decides whether to run the next stage based on the remaining
    // budget, not on this flag alone.
    virtual CompactOutcome evaluate(
        std::vector<Message>& messages,
        const CompactBudget& budget) = 0;
};

// ChainMetrics captures one full chain run for the trace sink.
// per_stage lists each stage's outcome; the chain itself emits a
// summary record through emit_latency_sample so the existing
// harness can filter on kStageCompactChain.
struct ChainMetrics {
    std::size_t bytes_before{0};
    std::size_t bytes_after{0};
    std::vector<CompactOutcome> per_stage;
};

// CompactChain owns an ordered list of stages. run() walks each
// stage once and stops when the budget is satisfied OR every stage
// returned triggered=false. The orchestrator does not chain
// stages; it runs each at most once per call. Reactive triggers
// (Ctrl+C long-press, hook events) call run() again separately.
class CompactChain {
public:
    void add_stage(std::unique_ptr<CompactStage> stage);
    const std::vector<std::unique_ptr<CompactStage>>& stages() const {
        return stages_;
    }
    ChainMetrics run(std::vector<Message>& messages,
                     const CompactBudget& budget,
                     const std::string& request_id);

private:
    std::vector<std::unique_ptr<CompactStage>> stages_;
};

// Convenience: measure the UTF-8 byte size of all messages. Used
// both to seed CompactBudget::current_bytes and to refresh the
// post-stage size in CompactOutcome.
std::size_t measure_message_bytes(
    const std::vector<Message>& messages);

}  // namespace agent::compact

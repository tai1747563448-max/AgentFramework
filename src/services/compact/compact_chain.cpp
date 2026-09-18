#include "services/compact/compact_chain.h"

#include <chrono>
#include <utility>

namespace agent::compact {

namespace {

// latency_us is a microsecond-resolution timer shared across all
// stages so each CompactOutcome's elapsed_us field lines up with
// the global trace samples the chain emits.
std::int64_t latency_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

void CompactChain::add_stage(std::unique_ptr<CompactStage> stage) {
    if (stage) stages_.push_back(std::move(stage));
}

ChainMetrics CompactChain::run(std::vector<Message>& messages,
                              const CompactBudget& budget,
                              const std::string& request_id) {
    ChainMetrics metrics;
    metrics.bytes_before = measure_message_bytes(messages);
    auto running_budget = budget;
    running_budget.current_bytes = metrics.bytes_before;
    for (auto& stage : stages_) {
        auto outcome = stage->evaluate(messages, running_budget);
        const auto freed = bytes_freed(outcome);
        // Refresh running budget so downstream stages see the
        // post-stage size and re-evaluate their trigger.
        const auto updated_size = measure_message_bytes(messages);
        outcome.bytes_after = updated_size;
        running_budget.current_bytes = updated_size;
        metrics.per_stage.push_back(outcome);
        emit_latency_sample(request_id, "compact_stage");
        if (running_budget.current_bytes <= running_budget.threshold_bytes) {
            break;
        }
        // Stop the chain when no stage produced savings so we do
        // not loop on a stuck budget.
        if (!outcome.triggered && freed == 0) {
            // No-op: do not advance past a stage that did nothing.
            // Future stages may still find work.
            continue;
        }
    }
    metrics.bytes_after = measure_message_bytes(messages);
    return metrics;
}

std::size_t measure_message_bytes(
    const std::vector<Message>& messages) {
    std::size_t total = 0;
    for (const auto& m : messages) {
        for (const auto& block : m.content) {
            std::visit(
                [&total](const auto& b) {
                    using B = std::decay_t<decltype(b)>;
                    if constexpr (std::is_same_v<B, TextBlock>) {
                        total += b.text.size();
                    } else if constexpr (std::is_same_v<B, ToolUseBlock>) {
                        total += b.call.id.size() +
                                 b.call.name.size() +
                                 64;  // rough estimate for arguments
                    } else if constexpr (std::is_same_v<B, ToolResultBlock>) {
                        total += b.result.tool_call_id.size() +
                                 b.result.content.size();
                    }
                },
                block);
        }
    }
    return total;
}

}  // namespace agent::compact

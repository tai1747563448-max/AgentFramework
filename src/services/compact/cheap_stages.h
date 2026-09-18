#pragma once

#include "services/compact/compact_chain.h"

#include <cstddef>

namespace agent::compact {

// ToolResultBudget caps a single oversized ToolResultBlock by
// trimming its content field down to max_bytes. The trigger fires
// when any single tool_result exceeds max_bytes; the stage does
// not touch tool_results that already fit. The new content keeps
// a "[truncated]" suffix so the model knows the rest was dropped.
class ToolResultBudget final : public CompactStage {
public:
    explicit ToolResultBudget(std::size_t max_bytes) : max_bytes_(max_bytes) {}
    std::string name() const override { return "tool_result_budget"; }
    CompactOutcome evaluate(
        std::vector<Message>& messages,
        const CompactBudget& budget) override;

private:
    std::size_t max_bytes_;
};

// SnipOldToolResults drops the oldest tool_result block from the
// middle of the conversation when the accumulated tool_result
// byte size exceeds budget.threshold_bytes. The most recent
// tool_result (which the model needs to keep reasoning about the
// current turn) is preserved.
class SnipOldToolResults final : public CompactStage {
public:
    std::string name() const override { return "snip_old_tool_results"; }
    CompactOutcome evaluate(
        std::vector<Message>& messages,
        const CompactBudget& budget) override;
};

// MicrocompactThinking drops "thinking" segments from text
// content blocks when their accumulated size exceeds
// threshold_bytes. The remaining text is preserved. This stage
// is a no-op when no thinking marker is present in any block.
class MicrocompactThinking final : public CompactStage {
public:
    explicit MicrocompactThinking(std::string marker = "<think>")
        : marker_(std::move(marker)) {}
    std::string name() const override { return "microcompact_thinking"; }
    CompactOutcome evaluate(
        std::vector<Message>& messages,
        const CompactBudget& budget) override;

private:
    std::string marker_;
};

// CollapseAdjacentMessages merges runs of same-role messages into
// a single message whose content is the concatenation of the run's
// blocks. Trigger fires when the run length is at least
// min_run_length (default 3), so a normal User/Assistant/User
// alternation is never collapsed.
class CollapseAdjacentMessages final : public CompactStage {
public:
    explicit CollapseAdjacentMessages(std::size_t min_run_length = 3)
        : min_run_length_(min_run_length) {}
    std::string name() const override { return "collapse_adjacent_messages"; }
    CompactOutcome evaluate(
        std::vector<Message>& messages,
        const CompactBudget& budget) override;

private:
    std::size_t min_run_length_;
};

}  // namespace agent::compact

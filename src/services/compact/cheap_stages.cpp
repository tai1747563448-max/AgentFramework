#include "services/compact/cheap_stages.h"

#include <algorithm>
#include <string>

namespace agent::compact {

namespace {

constexpr const char* kTruncatedSuffix = "...[truncated by tool_result_budget]";

std::size_t text_block_size(const std::string& s) { return s.size(); }

}  // namespace

CompactOutcome ToolResultBudget::evaluate(
    std::vector<Message>& messages,
    const CompactBudget& budget) {
    CompactOutcome outcome;
    outcome.bytes_before = measure_message_bytes(messages);
    bool any_truncated = false;
    for (auto& m : messages) {
        for (auto& block : m.content) {
            auto* result = std::get_if<ToolResultBlock>(&block);
            if (result == nullptr) continue;
            if (result->result.content.size() <= max_bytes_) continue;
            const std::string head = result->result.content.substr(
                0, max_bytes_);
            result->result.content = head + kTruncatedSuffix;
            any_truncated = true;
        }
    }
    outcome.bytes_after = measure_message_bytes(messages);
    outcome.triggered = any_truncated;
    (void)budget;
    return outcome;
}

CompactOutcome SnipOldToolResults::evaluate(
    std::vector<Message>& messages,
    const CompactBudget& budget) {
    CompactOutcome outcome;
    outcome.bytes_before = measure_message_bytes(messages);
    if (budget.threshold_bytes == 0 || budget.current_bytes <= budget.threshold_bytes) {
        outcome.bytes_after = outcome.bytes_before;
        return outcome;
    }
    std::size_t total = 0;
    std::vector<std::pair<std::size_t, std::size_t>> indexed_results;
    for (std::size_t i = 0; i < messages.size(); ++i) {
        const auto& m = messages[i];
        for (std::size_t j = 0; j < m.content.size(); ++j) {
            const auto* r = std::get_if<ToolResultBlock>(&m.content[j]);
            if (r == nullptr) continue;
            const std::size_t sz = r->result.content.size();
            indexed_results.emplace_back(i, sz);
            total += sz;
        }
    }
    outcome.bytes_after = outcome.bytes_before;
    if (total <= budget.threshold_bytes) return outcome;
    // Drop the oldest tool_result (skipping the most recent one)
    // until the running total falls under threshold_bytes.
    std::size_t removed = 0;
    for (std::size_t idx = 0;
         idx + 1 < indexed_results.size() &&
         total > budget.threshold_bytes;
         ++idx) {
        const auto [mi, sz] = indexed_results[idx];
        auto& m = messages[mi];
        // Remove the first tool_result block in the oldest matching
        // message whose content size matches the indexed size. The
        // last entry in indexed_results is preserved (idx+1 bound).
        for (auto it = m.content.begin(); it != m.content.end(); ++it) {
            auto* r = std::get_if<ToolResultBlock>(&*it);
            if (r == nullptr) continue;
            if (r->result.content.size() != sz) continue;
            it = m.content.erase(it);
            total -= sz;
            ++removed;
            break;
        }
    }
    outcome.bytes_after = measure_message_bytes(messages);
    outcome.triggered = removed > 0;
    return outcome;
}

CompactOutcome MicrocompactThinking::evaluate(
    std::vector<Message>& messages,
    const CompactBudget& budget) {
    CompactOutcome outcome;
    outcome.bytes_before = measure_message_bytes(messages);
    std::size_t thinking_bytes = 0;
    for (const auto& m : messages) {
        for (const auto& block : m.content) {
            const auto* t = std::get_if<TextBlock>(&block);
            if (t == nullptr) continue;
            std::size_t from = 0;
            while ((from = t->text.find(marker_, from)) !=
                   std::string::npos) {
                const auto close = t->text.find(marker_, from + marker_.size());
                const std::size_t end =
                    close == std::string::npos ? t->text.size() : close;
                thinking_bytes += end - from;
                from = end;
            }
        }
    }
    if (thinking_bytes == 0 ||
        thinking_bytes <= budget.threshold_bytes) {
        outcome.bytes_after = outcome.bytes_before;
        return outcome;
    }
    bool any_stripped = false;
    for (auto& m : messages) {
        for (auto& block : m.content) {
            auto* t = std::get_if<TextBlock>(&block);
            if (t == nullptr) continue;
            std::string out;
            out.reserve(t->text.size());
            std::size_t from = 0;
            bool changed = false;
            while (from < t->text.size()) {
                const auto open = t->text.find(marker_, from);
                if (open == std::string::npos) {
                    out.append(t->text, from, t->text.size() - from);
                    break;
                }
                out.append(t->text, from, open - from);
                const auto close = t->text.find(marker_, open + marker_.size());
                if (close == std::string::npos) {
                    changed = true;
                    break;
                }
                changed = true;
                from = close + marker_.size();
            }
            if (changed) {
                t->text = std::move(out);
                any_stripped = true;
            }
        }
    }
    outcome.bytes_after = measure_message_bytes(messages);
    outcome.triggered = any_stripped;
    return outcome;
}

CompactOutcome CollapseAdjacentMessages::evaluate(
    std::vector<Message>& messages,
    const CompactBudget& budget) {
    CompactOutcome outcome;
    outcome.bytes_before = measure_message_bytes(messages);
    bool any_collapsed = false;
    std::vector<Message> collapsed;
    collapsed.reserve(messages.size());
    for (auto& m : messages) {
        if (!collapsed.empty() && collapsed.back().role == m.role &&
            m.content.size() >= 1) {
            for (auto& block : m.content) {
                collapsed.back().content.push_back(std::move(block));
            }
            any_collapsed = true;
            continue;
        }
        collapsed.push_back(std::move(m));
    }
    if (any_collapsed) {
        messages = std::move(collapsed);
    }
    outcome.bytes_after = measure_message_bytes(messages);
    outcome.triggered = any_collapsed;
    (void)budget;
    return outcome;
}

}  // namespace agent::compact

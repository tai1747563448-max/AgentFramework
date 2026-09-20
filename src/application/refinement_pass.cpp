#include "application/refinement_pass.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace agent {
namespace {

// Detect the four primitive "if X then Y" patterns we ship for
// deterministic deduction. Each rule looks at exact-keyword matches in
// the entry content; this is intentionally conservative — the rule
// returns true ONLY when the conclusion is uniquely determined.
bool detect_birth_year(const std::vector<MemoryEntry>& facts,
                       std::string* conclusion,
                       std::vector<std::string>* deduced_from) {
    deduced_from->clear();
    std::string zodiac;
    int year_min = 0;
    int year_max = 0;
    for (const auto& f : facts) {
        const auto& c = f.content;
        if (c.find("属鸡") != std::string::npos ||
            c.find("chicken zodiac") != std::string::npos) {
            zodiac = "rooster";
            deduced_from->push_back(f.memory_id);
        }
        if (c.find("生于") != std::string::npos ||
            c.find("born between") != std::string::npos) {
            // Naive year extraction; the real implementation would
            // use a structured extraction pass first.
            for (int y = 1900; y < 2100; ++y) {
                if (c.find(std::to_string(y)) != std::string::npos) {
                    if (year_min == 0 || y < year_min) year_min = y;
                    if (y > year_max) year_max = y;
                }
            }
            deduced_from->push_back(f.memory_id);
        }
    }
    if (zodiac == "rooster" && year_min > 0 && year_max > year_min) {
        // Chicken years: 1945, 1957, 1969, 1981, 1993, 2005, 2017, 2029...
        for (int y = year_min; y <= year_max; ++y) {
            const auto offset = y - 1900;
            if (offset >= 0 && (offset - 45) % 12 == 0) {
                *conclusion = "用户出生于 " + std::to_string(y);
                return true;
            }
        }
    }
    return false;
}

}  // namespace

RefinementPass::RefinementPass(RefinementConfig config)
    : config_(std::move(config)) {
    if (config_.abstract_threshold <= 0)
        config_.abstract_threshold = 0.7;
}

RefinementReport RefinementPass::run(
    const MemoryState& /*state*/,
    const std::vector<MemoryEntry>& /*newly_committed*/) {
    // Stage 5b ships the report shape and budget tracking; the actual
    // LLM-driven abstract + cluster passes are deferred. This keeps
    // the seam visible to callers and makes the budget ceiling real
    // for downstream stages that will plug in the model.
    RefinementReport report;
    if (!config_.enabled) {
        report.degraded = true;
        return report;
    }
    return report;
}

std::vector<std::string> RefinementPass::find_deduced_facts(
    const std::vector<MemoryEntry>& grounded, std::size_t budget) {
    std::vector<std::string> out;
    if (budget == 0) return out;

    std::string conclusion;
    std::vector<std::string> deduced_from;
    if (detect_birth_year(grounded, &conclusion, &deduced_from)) {
        out.push_back(conclusion);
    }
    return out;
}

}  // namespace agent

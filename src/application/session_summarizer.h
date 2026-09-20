#pragma once

#include "domain/result.h"

#include <string>
#include <string_view>

namespace agent {

// T4 (stage 4): SessionSummarizer reduces a completed session's
// transcript into a short episodic summary that feeds the Period's
// summary pool. Stage 4 ships a deterministic extractor (head +
// tail truncation, max length) — calling out to the LLM happens later
// once we wire ModelClient without regressing the rest of the engine.
class SessionSummarizer {
public:
    struct Config {
        std::size_t max_chars{800};
        std::size_t head_chars{500};
        std::size_t tail_chars{300};
    };

    explicit SessionSummarizer(Config config = {});

    // Extract a summary from the raw transcript (free-form text —
    // messages are newline-separated). The result is guaranteed to
    // be <= max_chars.
    std::string summarize(std::string_view transcript) const;

private:
    Config config_;
};

}  // namespace agent

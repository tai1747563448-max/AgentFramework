#include "application/session_summarizer.h"

#include <algorithm>
#include <string>

namespace agent {

SessionSummarizer::SessionSummarizer(Config config)
    : config_(std::move(config)) {
    if (config_.max_chars == 0) config_.max_chars = 800;
    if (config_.head_chars == 0) config_.head_chars = 500;
    if (config_.tail_chars == 0) config_.tail_chars = 300;
}

std::string SessionSummarizer::summarize(std::string_view transcript) const {
    if (transcript.empty()) return {};
    if (transcript.size() <= config_.max_chars)
        return std::string(transcript);

    // Take head + tail + a marker so the reader sees both ends.
    const auto head_end = std::min(config_.head_chars, transcript.size());
    std::string head(transcript.substr(0, head_end));
    // If the head ended mid-word, trim back to a whitespace boundary.
    if (head_end < transcript.size()) {
        const auto last_ws = head.find_last_of(" \t\n");
        if (last_ws != std::string::npos && last_ws > head.size() / 2)
            head.resize(last_ws);
    }

    const auto tail_size = std::min(config_.tail_chars,
                                    transcript.size() - head_end);
    std::string tail(transcript.substr(transcript.size() - tail_size, tail_size));
    if (!tail.empty()) {
        const auto first_ws = tail.find_first_not_of(" \t\n");
        if (first_ws != std::string::npos)
            tail = tail.substr(first_ws);
    }

    return head + "\n... [truncated] ...\n" + tail;
}

}  // namespace agent

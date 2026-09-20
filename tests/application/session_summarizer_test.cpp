#include "application/session_summarizer.h"
#include "test_support.h"

#include <string>

namespace {

std::string long_transcript(std::size_t lines) {
    std::string out;
    for (std::size_t i = 0; i < lines; ++i) {
        out += "line ";
        out += std::to_string(i);
        out += ": the user asked something interesting about RAG architecture and ";
        out += "we discussed several design tradeoffs in detail\n";
    }
    return out;
}

}  // namespace

TEST_CASE(session_summarizer_returns_empty_for_empty_input) {
    const agent::SessionSummarizer s;
    REQUIRE(s.summarize("").empty());
}

TEST_CASE(session_summarizer_passes_through_short_input) {
    const agent::SessionSummarizer s;
    const std::string short_text = "short and sweet";
    REQUIRE(s.summarize(short_text) == short_text);
}

TEST_CASE(session_summarizer_truncates_long_input_with_marker) {
    agent::SessionSummarizer::Config cfg;
    cfg.max_chars = 200;
    cfg.head_chars = 80;
    cfg.tail_chars = 60;
    const agent::SessionSummarizer s(cfg);
    const auto out = s.summarize(long_transcript(20));
    REQUIRE(out.size() <= cfg.max_chars + 50);  // marker slack
    REQUIRE(out.find("[truncated]") != std::string::npos);
}

TEST_CASE(session_summarizer_preserves_distinct_head_and_tail) {
    agent::SessionSummarizer::Config cfg;
    cfg.max_chars = 200;
    cfg.head_chars = 60;
    cfg.tail_chars = 100;
    const agent::SessionSummarizer s(cfg);
    // Build a transcript where head and tail differ markedly.
    std::string transcript = "alpha beta gamma delta epsilon zeta eta theta iota kappa";
    for (int i = 0; i < 50; ++i) {
        transcript += " filler filler filler filler filler filler filler filler";
    }
    transcript += " omega the final chapter of this very long session about RAG";
    const auto out = s.summarize(transcript);
    REQUIRE(out.find("alpha") != std::string::npos);
    REQUIRE(out.find("omega") != std::string::npos);
}

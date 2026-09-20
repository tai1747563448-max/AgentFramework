#include "application/inverted_index.h"
#include "test_support.h"

#include <algorithm>
#include <cstdio>

TEST_CASE(inverted_index_starts_empty) {
    agent::InvertedIndex idx;
    REQUIRE(idx.size() == 0);
    REQUIRE(idx.candidates("anything").empty());
}

TEST_CASE(inverted_index_finds_entry_by_exact_token) {
    agent::InvertedIndex idx;
    idx.add("memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            "RAG architecture decision");
    const auto cands = idx.candidates("RAG");
    REQUIRE(cands.size() == 1);
    REQUIRE(cands[0] == "memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
}

TEST_CASE(inverted_index_returns_distinct_candidates) {
    agent::InvertedIndex idx;
    idx.add("memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            "alpha beta gamma");
    idx.add("memory-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
            "beta gamma delta");
    auto cands = idx.candidates("beta");
    REQUIRE(cands.size() == 2);
    std::sort(cands.begin(), cands.end());
    REQUIRE(cands[0] < cands[1]);
}

TEST_CASE(inverted_index_case_insensitive_lookup) {
    agent::InvertedIndex idx;
    idx.add("memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            "RAG architecture");
    REQUIRE(idx.candidates("rag").size() == 1);
    REQUIRE(idx.candidates("RAG").size() == 1);
}

TEST_CASE(inverted_index_remove_drops_only_target_entry) {
    agent::InvertedIndex idx;
    idx.add("memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "alpha beta");
    idx.add("memory-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "alpha gamma");
    idx.remove("memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    REQUIRE(idx.candidates("alpha").size() == 1);
    REQUIRE(idx.candidates("beta").empty());
    REQUIRE(idx.candidates("gamma").size() == 1);
}

TEST_CASE(inverted_index_re_add_refreshes_tokens) {
    agent::InvertedIndex idx;
    idx.add("memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "alpha beta");
    idx.add("memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "gamma delta");
    REQUIRE(idx.candidates("beta").empty());
    REQUIRE(idx.candidates("gamma").size() == 1);
    REQUIRE(idx.candidates("delta").size() == 1);
}

TEST_CASE(inverted_index_skips_single_byte_tokens) {
    agent::InvertedIndex idx;
    idx.add("memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "a b c");
    REQUIRE(idx.candidates("a").empty());
    REQUIRE(idx.candidates("b").empty());
}

TEST_CASE(inverted_index_handles_non_ascii_content) {
    agent::InvertedIndex idx;
    const std::string content =
        "\xe7\x94\xa8\xe6\x88\xb7\xe8\xae\xa8\xe8\xae\xba";
    idx.add("memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", content);
    // Query shares "用" (\xe7\x94\xa8) which is one UTF-8 char (3 bytes)
    // matching one of the indexed tokens.
    const std::string query = "\xe7\x94\xa8";
    const auto cands = idx.candidates(query);
    REQUIRE(!cands.empty());
}

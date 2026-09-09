#include "application/memory_retriever.h"
#include "application/memory_reducer.h"
#include "test_support.h"

#include <string>
#include <utility>
#include <vector>

namespace {
agent::MemoryEntry entry(std::string id, std::string scope, std::string content,
                         std::string updated = "2026-09-07T10:00:00.000Z") {
    return {std::move(id), agent::MemoryCategory::Fact, std::move(scope),
            std::move(content), "session-11111111111111111111111111111111", 1, 1,
            updated, std::move(updated),
            agent::MemoryOrigin::ExplicitUser};
}

std::string rendered(const std::string& id, const std::string& content) {
    return "- [" + id + "] (fact) " + content;
}
}  // namespace

TEST_CASE(memory_retriever_filters_workspace_and_omits_forgotten_entries) {
    const auto global = entry("memory-11111111111111111111111111111111", "",
                              "build tests before release",
                              "2026-09-07T10:00:01.000Z");
    const auto local = entry("memory-22222222222222222222222222222222", "E:/work",
                             "build tests before release");
    const auto forgotten = entry("memory-33333333333333333333333333333333", "E:/work",
                                 "build tests before release");
    const std::vector<agent::MemoryEvent> events{
        {1, 1, "2026-09-07T10:00:01.000Z", "one", agent::MemoryUpsertedPayload{global}},
        {1, 2, "2026-09-07T10:00:01.000Z", "two", agent::MemoryUpsertedPayload{local}},
        {1, 3, "2026-09-07T10:00:01.000Z", "three",
         agent::MemoryUpsertedPayload{forgotten}},
        {1, 4, "2026-09-07T10:00:01.000Z", "four",
         agent::MemoryForgottenPayload{forgotten.memory_id}}};
    const auto replayed = agent::replay_memory_events(events);
    REQUIRE(replayed.has_value());

    const agent::MemoryRetriever retriever;
    const auto result = retriever.retrieve(replayed.value(), "E:/work", "build tests", 5,
                                           4096);
    REQUIRE(result.has_value());
    REQUIRE((result.value() == std::vector<std::string>{
                                  rendered(global.memory_id, global.content),
                                  rendered(local.memory_id, local.content)}));
}

TEST_CASE(memory_retriever_ranks_chinese_and_ascii_phrases_with_stable_ties) {
    agent::MemoryState state;
    const auto phrase = entry("memory-11111111111111111111111111111111", "",
                              "build tests before release");
    const auto words = entry("memory-22222222222222222222222222222222", "",
                             "build reliable tests", "2026-09-07T11:00:00.000Z");
    const auto chinese = entry("memory-33333333333333333333333333333333", "",
                               "请使用中文回答问题");
    const auto tie_b = entry("memory-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "", "release",
                             "2026-09-07T12:00:00.000Z");
    const auto tie_a = entry("memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "", "release",
                             "2026-09-07T12:00:00.000Z");
    for (const auto& value : {phrase, words, chinese, tie_b, tie_a})
        state.active_entries.emplace(value.memory_id, value);

    const agent::MemoryRetriever retriever;
    const auto ascii = retriever.retrieve(state, "E:/work", "build tests", 5, 4096);
    REQUIRE(ascii.has_value());
    REQUIRE(ascii.value().at(0) == rendered(phrase.memory_id, phrase.content));
    REQUIRE(ascii.value().at(1) == rendered(words.memory_id, words.content));
    const auto chinese_result = retriever.retrieve(state, "E:/work", "中文问题", 5, 4096);
    REQUIRE(chinese_result.has_value());
    REQUIRE((chinese_result.value() == std::vector<std::string>{
                                         rendered(chinese.memory_id, chinese.content)}));
    const auto ties = retriever.retrieve(state, "E:/work", "release", 5, 4096);
    REQUIRE(ties.has_value());
    REQUIRE(ties.value().at(0) == rendered(tie_a.memory_id, tie_a.content));
    REQUIRE(ties.value().at(1) == rendered(tie_b.memory_id, tie_b.content));
}

TEST_CASE(memory_retriever_normalizes_equivalent_timestamp_ties_before_memory_id) {
    agent::MemoryState state;
    const auto milliseconds = entry("memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "",
                                    "equivalent", "2026-09-07T12:00:00.000Z");
    const auto seconds = entry("memory-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "",
                               "equivalent", "2026-09-07T12:00:00Z");
    state.active_entries.emplace(milliseconds.memory_id, milliseconds);
    state.active_entries.emplace(seconds.memory_id, seconds);

    const agent::MemoryRetriever retriever;
    const auto result = retriever.retrieve(state, "E:/work", "equivalent", 5, 4096);
    REQUIRE(result.has_value());
    REQUIRE(result.value().at(0) ==
            rendered(milliseconds.memory_id, milliseconds.content));
    REQUIRE(result.value().at(1) == rendered(seconds.memory_id, seconds.content));
}

TEST_CASE(memory_retriever_enforces_limits_omits_zero_scores_and_rejects_invalid_utf8) {
    agent::MemoryState state;
    const auto first = entry("memory-11111111111111111111111111111111", "", "alpha");
    const auto second = entry("memory-22222222222222222222222222222222", "", "alpha");
    const auto unrelated = entry("memory-33333333333333333333333333333333", "", "beta");
    for (const auto& value : {first, second, unrelated})
        state.active_entries.emplace(value.memory_id, value);
    const agent::MemoryRetriever retriever;

    const auto top_one = retriever.retrieve(state, "E:/work", "alpha", 1, 4096);
    REQUIRE(top_one.has_value());
    REQUIRE(top_one.value().size() == 1);
    const auto exact = rendered(first.memory_id, first.content);
    const auto budget = retriever.retrieve(state, "E:/work", "alpha", 5, exact.size());
    REQUIRE(budget.has_value());
    REQUIRE(budget.value() == std::vector<std::string>{exact});
    const auto zero = retriever.retrieve(state, "E:/work", "gamma", 5, 4096);
    REQUIRE(zero.has_value());
    REQUIRE(zero.value().empty());
    const auto invalid = retriever.retrieve(state, "E:/work", "\xFF", 5, 4096);
    REQUIRE(!invalid.has_value());
    REQUIRE(invalid.error().message == "memory retrieval input is invalid");
}

TEST_CASE(memory_retriever_counts_inter_line_separator_in_exact_budget) {
    agent::MemoryState state;
    const auto first = entry("memory-11111111111111111111111111111111", "", "alpha");
    const auto second = entry("memory-22222222222222222222222222222222", "", "alpha");
    state.active_entries.emplace(first.memory_id, first);
    state.active_entries.emplace(second.memory_id, second);
    const agent::MemoryRetriever retriever;

    // Each rendered line is 56 bytes. Joining both requires 56 + 1 + 56.
    const auto no_separator_room = retriever.retrieve(state, "E:/work", "alpha", 2, 112);
    REQUIRE(no_separator_room.has_value());
    REQUIRE(no_separator_room.value().size() == 1);
    REQUIRE(no_separator_room.value()[0] == rendered(first.memory_id, first.content));

    const auto exact_joined_budget = retriever.retrieve(state, "E:/work", "alpha", 2, 113);
    REQUIRE(exact_joined_budget.has_value());
    REQUIRE(exact_joined_budget.value().size() == 2);
    const auto joined = exact_joined_budget.value()[0] + '\n' + exact_joined_budget.value()[1];
    REQUIRE(joined.size() == 113);
}

TEST_CASE(memory_retriever_does_not_match_unicode_punctuation_or_whitespace) {
    agent::MemoryState state;
    const auto punctuation = entry("memory-11111111111111111111111111111111", "",
                                   "偏好中文！？　");
    const auto unrelated = entry("memory-22222222222222222222222222222222", "",
                                 "请使用中文回答？");
    state.active_entries.emplace(punctuation.memory_id, punctuation);
    state.active_entries.emplace(unrelated.memory_id, unrelated);
    const agent::MemoryRetriever retriever;

    const auto punctuation_only =
        retriever.retrieve(state, "E:/work", "！？　", 5, 4096);
    REQUIRE(punctuation_only.has_value());
    REQUIRE(punctuation_only.value().empty());

    const auto unrelated_chat =
        retriever.retrieve(state, "E:/work", "今天天气如何？", 5, 4096);
    REQUIRE(unrelated_chat.has_value());
    REQUIRE(unrelated_chat.value().empty());
}

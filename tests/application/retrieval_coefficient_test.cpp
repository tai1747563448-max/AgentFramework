#include "application/retrieval_coefficient.h"
#include "domain/memory_state.h"
#include "test_support.h"

#include <cmath>
#include <vector>

namespace {

agent::MemoryEntry make_entry(const std::string& id) {
    agent::MemoryEntry e;
    e.memory_id = id;
    e.category = agent::MemoryCategory::Fact;
    e.content = "placeholder for " + id;
    e.created_at_utc = "2026-09-15T10:00:00.000Z";
    e.updated_at_utc = "2026-09-15T10:00:00.000Z";
    return e;
}

}  // namespace

TEST_CASE(retrieval_coefficient_mean_is_one_over_count) {
    REQUIRE(agent::RetrievalCoefficientAllocator::mean_coefficient(0) == 0.0);
    REQUIRE(agent::RetrievalCoefficientAllocator::mean_coefficient(1) == 1.0);
    REQUIRE(std::abs(agent::RetrievalCoefficientAllocator::mean_coefficient(4) -
                     0.25) < 1e-9);
}

TEST_CASE(retrieval_coefficient_decay_factor_grows_with_access_count) {
    agent::RetrievalCoefficientConfig cfg;
    const auto low = agent::RetrievalCoefficientAllocator::decay_factor(0, cfg);
    const auto mid = agent::RetrievalCoefficientAllocator::decay_factor(5, cfg);
    const auto high = agent::RetrievalCoefficientAllocator::decay_factor(50, cfg);
    // Higher access_count -> decay closer to 1 (slower decay).
    REQUIRE(low < mid);
    REQUIRE(mid < high);
    REQUIRE(high <= 1.0);
}

TEST_CASE(retrieval_coefficient_allocate_sums_to_one) {
    agent::RetrievalCoefficientAllocator alloc;
    std::vector<agent::MemoryEntry> entries{
        make_entry("memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        make_entry("memory-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"),
        make_entry("memory-cccccccccccccccccccccccccccccccc")};
    alloc.on_commit(entries, {"memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"});

    double total = 0;
    for (const auto& e : entries) total += e.retrieval_coefficient;
    REQUIRE(std::abs(total - 1.0) < 1e-9);
}

TEST_CASE(retrieval_coefficient_new_fact_gets_boost) {
    agent::RetrievalCoefficientConfig cfg;
    cfg.curate_boost_periods = 3;
    cfg.curate_boost_amount = 0.05;
    agent::RetrievalCoefficientAllocator alloc(cfg);
    std::vector<agent::MemoryEntry> entries{
        make_entry("memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        make_entry("memory-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb")};
    alloc.on_commit(entries, {"memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"});

    bool found_new_with_boost = false;
    for (const auto& e : entries) {
        if (e.memory_id == "memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") {
            REQUIRE(e.curate_boost_remaining == 3);
            found_new_with_boost = true;
        }
    }
    REQUIRE(found_new_with_boost);
}

TEST_CASE(retrieval_coefficient_normalize_drifts_toward_mean) {
    agent::RetrievalCoefficientAllocator alloc;
    std::vector<agent::MemoryEntry> entries{
        make_entry("memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        make_entry("memory-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb")};
    // Start: equal weights.
    entries[0].retrieval_coefficient = 0.9;
    entries[1].retrieval_coefficient = 0.1;
    alloc.normalize(entries);
    // Both should drift toward 0.5.
    REQUIRE(entries[0].retrieval_coefficient < 0.9);
    REQUIRE(entries[0].retrieval_coefficient > 0.5);
    REQUIRE(entries[1].retrieval_coefficient > 0.1);
    REQUIRE(entries[1].retrieval_coefficient < 0.5);
    // Sum stays at 1.
    double total = 0;
    for (const auto& e : entries) total += e.retrieval_coefficient;
    REQUIRE(std::abs(total - 1.0) < 1e-9);
}

TEST_CASE(retrieval_coefficient_on_retrieval_increments_and_lowers_difficulty) {
    agent::RetrievalCoefficientConfig cfg;
    cfg.per_retrieval_decrement = 0.1;
    agent::RetrievalCoefficientAllocator alloc(cfg);
    auto entry = make_entry("memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    entry.retrieval_difficulty = 0.8;
    entry.access_count = 0;
    alloc.on_retrieval(entry);
    REQUIRE(entry.access_count == 1);
    REQUIRE(std::abs(entry.retrieval_difficulty - 0.7) < 1e-9);
    alloc.on_retrieval(entry);
    REQUIRE(entry.access_count == 2);
    REQUIRE(std::abs(entry.retrieval_difficulty - 0.6) < 1e-9);
}

TEST_CASE(retrieval_coefficient_difficulty_floors_at_zero) {
    agent::RetrievalCoefficientConfig cfg;
    cfg.per_retrieval_decrement = 0.5;
    agent::RetrievalCoefficientAllocator alloc(cfg);
    auto entry = make_entry("memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    entry.retrieval_difficulty = 0.3;
    alloc.on_retrieval(entry);
    REQUIRE(entry.retrieval_difficulty == 0.0);
}

#include "services/cost_tracker.h"
#include "test_support.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

class CostTrackerFixture {
public:
    CostTrackerFixture() : path_(std::filesystem::temp_directory_path() /
        ("usage-" + std::to_string(counter_++) + ".jsonl")) {}
    ~CostTrackerFixture() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
    static inline int counter_{0};
};

TEST_CASE(cost_tracker_records_single_call) {
    CostTrackerFixture fixture;
    agent::CostTrackerSink sink(fixture.path());
    sink.record("task-00112233445566778899aabbccddeeff",
                "claude-3-5-sonnet", 1000, 500, 0.0105, true);
    sink.flush();
    agent::CostTrackerSink drain(fixture.path());
    const auto aggregate = drain.read_aggregate();
    REQUIRE(aggregate.calls == 1u);
    REQUIRE(aggregate.input_tokens == 1000u);
    REQUIRE(aggregate.output_tokens == 500u);
    REQUIRE(aggregate.usd > 0.009);
    REQUIRE(aggregate.usd < 0.012);
}

TEST_CASE(cost_tracker_records_failed_call) {
    CostTrackerFixture fixture;
    agent::CostTrackerSink sink(fixture.path());
    sink.record("task-00112233445566778899aabbccddeeff",
                "claude-3-5-sonnet", 0, 0, 0.0, false);
    sink.flush();
    agent::CostTrackerSink drain(fixture.path());
    const auto aggregate = drain.read_aggregate();
    REQUIRE(aggregate.calls == 1u);
    REQUIRE(aggregate.input_tokens == 0u);
}

TEST_CASE(cost_tracker_aggregates_multiple_records) {
    CostTrackerFixture fixture;
    agent::CostTrackerSink sink(fixture.path());
    for (int i = 0; i < 3; ++i) {
        sink.record("task-00112233445566778899aabbccddeeff",
                    "claude-3-5-sonnet", 1000, 500, 0.0105, true);
    }
    sink.flush();
    agent::CostTrackerSink drain(fixture.path());
    const auto aggregate = drain.read_aggregate();
    REQUIRE(aggregate.calls == 3u);
    REQUIRE(aggregate.input_tokens == 3000u);
    REQUIRE(aggregate.output_tokens == 1500u);
    REQUIRE(aggregate.usd > 0.030);
    REQUIRE(aggregate.usd < 0.040);
}

TEST_CASE(cost_tracker_zero_usd_for_unknown_model) {
    CostTrackerFixture fixture;
    agent::CostTrackerSink sink(fixture.path());
    sink.record("task-00112233445566778899aabbccddeeff",
                "unknown-model", 1000, 500, 0.0, true);
    sink.flush();
    agent::CostTrackerSink drain(fixture.path());
    const auto aggregate = drain.read_aggregate();
    REQUIRE(aggregate.calls == 1u);
    REQUIRE(aggregate.usd == 0.0);
}

TEST_CASE(cost_tracker_hook_is_fail_safe) {
    CostTrackerFixture fixture;
    agent::CostTrackerSink sink(fixture.path());
    auto hook = agent::make_cost_tracker_hook(
        sink, [] { return std::string("claude-3-5-sonnet"); });
    agent::HookPostModelCall event{
        "session-00112233445566778899aabbccddeeff",
        "task-00112233445566778899aabbccddeeff",
        nullptr,
        false};
    // response is null; the hook must not throw.
    bool threw = true;
    try { hook(event); threw = false; } catch (...) {}
    REQUIRE(!threw);
}

TEST_CASE(cost_tracker_appends_one_jsonl_record_per_call) {
    CostTrackerFixture fixture;
    agent::CostTrackerSink sink(fixture.path());
    sink.record("task-00112233445566778899aabbccddeeff",
                "claude-3-5-sonnet", 100, 50, 0.001, true);
    sink.flush();
    std::ifstream input(fixture.path(), std::ios::binary);
    REQUIRE(input.is_open());
    std::string line;
    std::getline(input, line);
    REQUIRE(!line.empty());
    REQUIRE(line.front() == '{');
    REQUIRE(line.find("\"task_id\"") != std::string::npos);
    REQUIRE(line.find("\"input\":100") != std::string::npos);
    REQUIRE(line.find("\"output\":50") != std::string::npos);
}

}  // namespace
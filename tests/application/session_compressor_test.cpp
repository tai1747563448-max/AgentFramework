#include "adapters/persistence/jsonl_period_store.h"
#include "application/period_manager.h"
#include "application/session_compressor.h"
#include "application/session_summarizer.h"
#include "ports/clock.h"
#include "ports/id_generator.h"
#include "test_support.h"

#include <cstdint>
#include <string>

namespace {

class FixedClock final : public agent::Clock {
public:
    std::string now_utc() const override { return "2026-09-15T10:00:00.000Z"; }
    std::int64_t monotonic_ms() const override { return 0; }
};

class CounterIdGenerator final : public agent::IdGenerator {
public:
    std::string next_task_id() override { return "task-" + std::to_string(++counter); }
    std::string next_correlation_id() override {
        return "period-" + std::to_string(++counter) +
               "aaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    }
private:
    std::uint64_t counter{0};
};

}  // namespace

TEST_CASE(session_compressor_attaches_and_appends_summary) {
    agent::JsonlPeriodStore store("runtime");
    FixedClock clock;
    CounterIdGenerator ids;
    agent::PeriodManager mgr(store, clock, ids);
    agent::SessionSummarizer summarizer;
    agent::SessionCompressor compressor(mgr, summarizer);

    const auto period = mgr.open_period().value();
    const auto session = std::string("session-11111111111111111111111111111111");

    const auto result = compressor.compress(
        session, period.period_id,
        "user asked about RAG architecture and decided to make it core");
    REQUIRE(result.has_value());
    REQUIRE(result.value().session_id == session);
    REQUIRE(result.value().period_id == period.period_id);
    REQUIRE(!result.value().summary.empty());

    // Side effect: session attached + summary appended.
    const auto found = mgr.find(period.period_id).value();
    REQUIRE(found.has_value());
    REQUIRE(found.value().session_ids.size() == 1);
    REQUIRE(found.value().episodic_summary.find("RAG") != std::string::npos);
}

TEST_CASE(session_compressor_rejects_empty_session_id) {
    agent::JsonlPeriodStore store("runtime");
    FixedClock clock;
    CounterIdGenerator ids;
    agent::PeriodManager mgr(store, clock, ids);
    agent::SessionSummarizer summarizer;
    agent::SessionCompressor compressor(mgr, summarizer);
    const auto period = mgr.open_period().value();

    const auto result = compressor.compress(
        "", period.period_id, "anything");
    REQUIRE(!result.has_value());
}

TEST_CASE(session_compressor_rejects_unknown_period) {
    agent::JsonlPeriodStore store("runtime");
    FixedClock clock;
    CounterIdGenerator ids;
    agent::PeriodManager mgr(store, clock, ids);
    agent::SessionSummarizer summarizer;
    agent::SessionCompressor compressor(mgr, summarizer);

    const auto result = compressor.compress(
        "session-11111111111111111111111111111111",
        "period-doesnotexist",
        "anything");
    REQUIRE(!result.has_value());
}

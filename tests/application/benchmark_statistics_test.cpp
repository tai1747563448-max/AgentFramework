#include "application/benchmark_statistics.h"
#include "test_support.h"

#include <cmath>
#include <limits>
#include <vector>

namespace {

bool near(double actual, double expected, double tolerance = 1e-9) {
    return std::abs(actual - expected) <= tolerance;
}

}  // namespace

TEST_CASE(benchmark_summary_reports_hand_checked_latency_and_reliability) {
    const std::vector<agent::BenchmarkObservation> observations{
        {10.0, true}, {20.0, true}, {30.0, false}, {40.0, true}};

    const auto summarized = agent::summarize_benchmark(observations);

    REQUIRE(summarized.has_value());
    const auto& summary = summarized.value();
    REQUIRE(summary.sample_count == 4);
    REQUIRE(summary.success_count == 3);
    REQUIRE(summary.error_count == 1);
    REQUIRE(near(summary.total_duration_us, 100.0));
    REQUIRE(near(summary.min_latency_us, 10.0));
    REQUIRE(near(summary.mean_latency_us, 25.0));
    REQUIRE(near(summary.p50_latency_us, 25.0));
    REQUIRE(near(summary.p95_latency_us, 38.5));
    REQUIRE(near(summary.p99_latency_us, 39.7));
    REQUIRE(near(summary.max_latency_us, 40.0));
    REQUIRE(near(summary.throughput_ops_per_second, 40'000.0));
    REQUIRE(near(summary.success_rate, 0.75));
}

TEST_CASE(benchmark_summary_rejects_empty_or_nonpositive_nonfinite_latency) {
    REQUIRE(!agent::summarize_benchmark({}).has_value());
    REQUIRE(!agent::summarize_benchmark({{0.0, true}}).has_value());
    REQUIRE(!agent::summarize_benchmark({{-1.0, true}}).has_value());
    REQUIRE(!agent::summarize_benchmark(
                 {{std::numeric_limits<double>::infinity(), true}})
                 .has_value());
    REQUIRE(!agent::summarize_benchmark(
                 {{std::numeric_limits<double>::quiet_NaN(), true}})
                 .has_value());
}

TEST_CASE(benchmark_comparison_flags_only_policy_violations) {
    agent::BenchmarkSummary baseline;
    baseline.p95_latency_us = 100.0;
    baseline.throughput_ops_per_second = 1'000.0;

    agent::BenchmarkSummary boundary;
    boundary.p95_latency_us = 110.0;
    boundary.throughput_ops_per_second = 900.0;
    boundary.success_rate = 0.99;

    const agent::BenchmarkRegressionPolicy policy{10.0, 10.0, 0.99};
    const auto accepted =
        agent::compare_benchmark(baseline, boundary, policy);
    REQUIRE(accepted.has_value());
    REQUIRE(accepted.value().passed);
    REQUIRE(!accepted.value().latency_regressed);
    REQUIRE(!accepted.value().throughput_regressed);
    REQUIRE(!accepted.value().success_rate_below_minimum);
    REQUIRE(near(accepted.value().p95_latency_change_percent, 10.0));
    REQUIRE(near(accepted.value().throughput_drop_percent, 10.0));

    auto regressed = boundary;
    regressed.p95_latency_us = 110.01;
    regressed.throughput_ops_per_second = 899.9;
    regressed.success_rate = 0.98;
    const auto rejected =
        agent::compare_benchmark(baseline, regressed, policy);
    REQUIRE(rejected.has_value());
    REQUIRE(!rejected.value().passed);
    REQUIRE(rejected.value().latency_regressed);
    REQUIRE(rejected.value().throughput_regressed);
    REQUIRE(rejected.value().success_rate_below_minimum);
}

TEST_CASE(benchmark_comparison_rejects_invalid_baseline_or_policy) {
    agent::BenchmarkSummary invalid_baseline;
    invalid_baseline.p95_latency_us = 0.0;
    invalid_baseline.throughput_ops_per_second = 100.0;

    agent::BenchmarkSummary current;
    current.p95_latency_us = 10.0;
    current.throughput_ops_per_second = 100.0;
    current.success_rate = 1.0;

    REQUIRE(!agent::compare_benchmark(
                 invalid_baseline, current, {10.0, 10.0, 1.0})
                 .has_value());
    REQUIRE(!agent::compare_benchmark(
                 current, current, {-1.0, 10.0, 1.0})
                 .has_value());
    REQUIRE(!agent::compare_benchmark(
                 current, current, {10.0, 10.0, 1.01})
                 .has_value());
}

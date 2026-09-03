#include "application/benchmark_statistics.h"

#include "domain/runtime_error.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace agent {
namespace {

Result<BenchmarkSummary> invalid_summary() {
    return Result<BenchmarkSummary>::failure(
        {ErrorCode::InvalidInput, "invalid benchmark observations", false});
}

Result<BenchmarkComparison> invalid_comparison() {
    return Result<BenchmarkComparison>::failure(
        {ErrorCode::InvalidInput, "invalid benchmark comparison", false});
}

double percentile(const std::vector<double>& sorted, double quantile) {
    const double position =
        static_cast<double>(sorted.size() - 1) * quantile;
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = static_cast<std::size_t>(std::ceil(position));
    if (lower == upper) {
        return sorted[lower];
    }
    const double fraction = position - static_cast<double>(lower);
    return sorted[lower] + (sorted[upper] - sorted[lower]) * fraction;
}

bool finite_positive(double value) {
    return std::isfinite(value) && value > 0.0;
}

}  // namespace

Result<BenchmarkSummary> summarize_benchmark(
    const std::vector<BenchmarkObservation>& observations) {
    if (observations.empty()) {
        return invalid_summary();
    }

    std::vector<double> latencies;
    latencies.reserve(observations.size());
    std::size_t operation_count = 0;
    std::size_t success_count = 0;
    double total_duration_us = 0.0;
    for (const auto& observation : observations) {
        if (!finite_positive(observation.duration_us) ||
            observation.operation_count == 0 ||
            observation.success_count > observation.operation_count ||
            observation.operation_count >
                std::numeric_limits<std::size_t>::max() - operation_count) {
            return invalid_summary();
        }
        const double latency_us =
            observation.duration_us /
            static_cast<double>(observation.operation_count);
        if (!finite_positive(latency_us)) {
            return invalid_summary();
        }
        latencies.push_back(latency_us);
        total_duration_us += observation.duration_us;
        if (!std::isfinite(total_duration_us)) {
            return invalid_summary();
        }
        operation_count += observation.operation_count;
        success_count += observation.success_count;
    }

    std::sort(latencies.begin(), latencies.end());
    BenchmarkSummary summary;
    summary.sample_count = observations.size();
    summary.operation_count = operation_count;
    summary.success_count = success_count;
    summary.error_count = operation_count - success_count;
    summary.total_duration_us = total_duration_us;
    summary.min_latency_us = latencies.front();
    summary.mean_latency_us =
        total_duration_us / static_cast<double>(operation_count);
    summary.p50_latency_us = percentile(latencies, 0.50);
    summary.p95_latency_us = percentile(latencies, 0.95);
    summary.p99_latency_us = percentile(latencies, 0.99);
    summary.max_latency_us = latencies.back();
    summary.throughput_ops_per_second =
        static_cast<double>(operation_count) * 1'000'000.0 /
        total_duration_us;
    summary.success_rate =
        static_cast<double>(success_count) /
        static_cast<double>(operation_count);
    return Result<BenchmarkSummary>::success(std::move(summary));
}

Result<BenchmarkComparison> compare_benchmark(
    const BenchmarkSummary& baseline,
    const BenchmarkSummary& current,
    const BenchmarkRegressionPolicy& policy) {
    if (!finite_positive(baseline.p95_latency_us) ||
        !finite_positive(baseline.throughput_ops_per_second) ||
        !finite_positive(current.p95_latency_us) ||
        !finite_positive(current.throughput_ops_per_second) ||
        !std::isfinite(current.success_rate) || current.success_rate < 0.0 ||
        current.success_rate > 1.0 ||
        !std::isfinite(policy.max_p95_latency_regression_percent) ||
        policy.max_p95_latency_regression_percent < 0.0 ||
        !std::isfinite(policy.max_throughput_regression_percent) ||
        policy.max_throughput_regression_percent < 0.0 ||
        !std::isfinite(policy.minimum_success_rate) ||
        policy.minimum_success_rate < 0.0 || policy.minimum_success_rate > 1.0) {
        return invalid_comparison();
    }

    BenchmarkComparison comparison;
    comparison.p95_latency_change_percent =
        (current.p95_latency_us - baseline.p95_latency_us) /
        baseline.p95_latency_us * 100.0;
    comparison.throughput_drop_percent =
        (baseline.throughput_ops_per_second -
         current.throughput_ops_per_second) /
        baseline.throughput_ops_per_second * 100.0;

    constexpr double kComparisonTolerance = 1e-9;
    comparison.latency_regressed =
        comparison.p95_latency_change_percent >
        policy.max_p95_latency_regression_percent + kComparisonTolerance;
    comparison.throughput_regressed =
        comparison.throughput_drop_percent >
        policy.max_throughput_regression_percent + kComparisonTolerance;
    comparison.success_rate_below_minimum =
        current.success_rate + kComparisonTolerance <
        policy.minimum_success_rate;
    comparison.passed = !comparison.latency_regressed &&
                        !comparison.throughput_regressed &&
                        !comparison.success_rate_below_minimum;
    return Result<BenchmarkComparison>::success(std::move(comparison));
}

}  // namespace agent

#pragma once

#include "domain/result.h"

#include <cstddef>
#include <vector>

namespace agent {

struct BenchmarkObservation {
    double duration_us{0.0};
    bool succeeded{false};
};

struct BenchmarkSummary {
    std::size_t sample_count{0};
    std::size_t success_count{0};
    std::size_t error_count{0};
    double total_duration_us{0.0};
    double min_latency_us{0.0};
    double mean_latency_us{0.0};
    double p50_latency_us{0.0};
    double p95_latency_us{0.0};
    double p99_latency_us{0.0};
    double max_latency_us{0.0};
    double throughput_ops_per_second{0.0};
    double success_rate{0.0};
};

struct BenchmarkRegressionPolicy {
    double max_p95_latency_regression_percent{10.0};
    double max_throughput_regression_percent{10.0};
    double minimum_success_rate{1.0};
};

struct BenchmarkComparison {
    bool passed{false};
    bool latency_regressed{false};
    bool throughput_regressed{false};
    bool success_rate_below_minimum{false};
    double p95_latency_change_percent{0.0};
    double throughput_drop_percent{0.0};
};

Result<BenchmarkSummary> summarize_benchmark(
    const std::vector<BenchmarkObservation>& observations);

Result<BenchmarkComparison> compare_benchmark(
    const BenchmarkSummary& baseline,
    const BenchmarkSummary& current,
    const BenchmarkRegressionPolicy& policy);

}  // namespace agent

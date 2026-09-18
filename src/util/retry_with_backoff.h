#pragma once

#include "domain/latency_trace.h"
#include "ports/cancellation.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <utility>

namespace agent {

// BackoffPolicy captures the retry tuning for an idempotent call. The
// defaults match the T02 (v2 §1) spec: base 500ms, cap 8s, full-jitter
// window of ±20%, and a maximum of 3 attempts (the original call plus
// two retries).
struct BackoffPolicy {
    std::chrono::milliseconds base{500};
    std::chrono::milliseconds cap{8'000};
    // Jitter is a fraction (0.2 means ±20%) applied symmetrically
    // around the computed backoff.
    double jitter{0.2};
    std::size_t max_attempts{3};
};

// OnRetryFn is invoked once per retry (after the first failed attempt
// and before the next sleep). It receives the 1-based attempt number
// and the deadline the next call will use so the call site can emit
// trace samples with a consistent monotonic reference.
using OnRetryFn = std::function<void(std::size_t attempt,
                                    std::chrono::steady_clock::time_point
                                        next_deadline)>;

// retry_with_backoff runs `attempt` until it either succeeds, returns
// an error whose retryable flag is false, exhausts max_attempts, or
// observes a cancellation request. The function returns the final
// result so the caller can surface it as-is; attempts are counted from
// 1 and the first retry happens after the first failure.
//
// The backoff schedule is exponential: attempt N sleeps for
// min(base * 2^(N-1), cap) before applying ±jitter% noise. Negative
// values are clamped to zero so an exhausted budget never sleeps.
template <typename AttemptFn>
auto retry_with_backoff(const BackoffPolicy& policy,
                       const Cancellation* cancellation,
                       AttemptFn&& attempt,
                       OnRetryFn on_retry = nullptr) ->
    typename std::invoke_result<AttemptFn>::type {
    using Result = typename std::invoke_result<AttemptFn>::type;

    std::random_device rd;
    std::mt19937 generator(rd());

    std::size_t attempt_number = 0;
    while (true) {
        ++attempt_number;
        Result result = attempt();
        // Non-retryable outcomes short-circuit immediately.
        if (result.has_value() || !result.error().retryable ||
            attempt_number >= policy.max_attempts) {
            return result;
        }
        if (cancellation != nullptr && cancellation->requested()) {
            return result;
        }
        const std::int64_t raw_ms =
            std::min<std::int64_t>(
                policy.base.count() *
                    static_cast<std::int64_t>(1ULL << (attempt_number - 1)),
                policy.cap.count());
        const double jitter_window = static_cast<double>(raw_ms) * policy.jitter;
        std::uniform_real_distribution<double> jitter_distribution(
            -jitter_window, jitter_window);
        const std::int64_t jitter_ms = static_cast<std::int64_t>(
            jitter_distribution(generator));
        const std::int64_t sleep_ms = std::max<std::int64_t>(
            0, raw_ms + jitter_ms);
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(sleep_ms);
        if (on_retry) {
            on_retry(attempt_number, deadline);
        }
        std::this_thread::sleep_until(deadline);
    }
}

// Convenience stage identifier for T02 (v2 §1). Keep aligned with the
// harness' stage filter; see benchmarks/cli_latency_benchmark.py.
inline constexpr const char* kStageProviderRetry = "provider_retry";

}  // namespace agent

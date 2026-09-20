#include "adapters/system/signal_cancellation.h"

#include <atomic>
#include <csignal>

namespace agent {
namespace {

std::atomic_bool cancellation_requested{false};
static_assert(std::atomic_bool::is_always_lock_free,
              "the signal flag must support signal-safe lock-free stores");

void handle_sigint(int) noexcept {
    // Windows resets the CRT handler when it handles SIGINT.
    std::signal(SIGINT, &handle_sigint);
    cancellation_requested.store(true, std::memory_order_relaxed);
}

}  // namespace

SignalCancellation::SignalCancellation() {
    begin_turn();
}

bool SignalCancellation::is_cancelled() const noexcept {
    return cancellation_requested.load(std::memory_order_relaxed);
}

void SignalCancellation::begin_turn() noexcept {
    cancellation_requested.store(false, std::memory_order_relaxed);
    std::signal(SIGINT, &handle_sigint);
}

void SignalCancellation::end_turn() noexcept {
    std::signal(SIGINT, SIG_DFL);
}

void SignalCancellation::cancel() noexcept {
    cancellation_requested.store(true, std::memory_order_relaxed);
}

}  // namespace agent

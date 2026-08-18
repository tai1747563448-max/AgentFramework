#include "adapters/system/signal_cancellation.h"

#include <atomic>
#include <csignal>
#include <mutex>

namespace agent {
namespace {

std::atomic_bool cancellation_requested{false};

void handle_sigint(int) noexcept {
    cancellation_requested.store(true, std::memory_order_relaxed);
}

}  // namespace

SignalCancellation::SignalCancellation() {
    static std::once_flag install_once;
    std::call_once(install_once,
                   [] { std::signal(SIGINT, &handle_sigint); });
}

bool SignalCancellation::requested() const noexcept {
    return cancellation_requested.load(std::memory_order_relaxed);
}

}  // namespace agent

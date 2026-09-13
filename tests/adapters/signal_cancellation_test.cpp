#include "adapters/system/signal_cancellation.h"
#include "test_support.h"

#include <csignal>

TEST_CASE(signal_cancellation_second_turn_resets_cancelled_first_turn) {
    agent::SignalCancellation cancellation;
    cancellation.begin_turn();
    REQUIRE(!cancellation.requested());
    cancellation.request_cancel();
    REQUIRE(cancellation.requested());
    cancellation.end_turn();
    cancellation.begin_turn();
    REQUIRE(!cancellation.requested());
    std::raise(SIGINT);
    REQUIRE(cancellation.requested());
    cancellation.end_turn();
}

TEST_CASE(signal_cancellation_idle_restores_default_sigint_action) {
    agent::SignalCancellation cancellation;
    cancellation.begin_turn();
    cancellation.end_turn();
    const auto previous = std::signal(SIGINT, SIG_DFL);
    REQUIRE(previous == SIG_DFL);
}

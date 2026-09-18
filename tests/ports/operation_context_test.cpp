#include "ports/operation_context.h"

#include "test_support.h"

#include <chrono>
#include <thread>

namespace {

class CountingCancellation final : public agent::Cancellation {
public:
    explicit CountingCancellation(bool requested) : requested_(requested) {}
    bool requested() const noexcept override { return requested_; }
private:
    bool requested_;
};

}  // namespace

TEST_CASE(OperationContextReportsCancellation) {
    CountingCancellation cancel(true);
    const agent::OperationContext context{&cancel,
        std::chrono::steady_clock::now() + std::chrono::seconds(60)};
    REQUIRE(context.cancelled());
    REQUIRE(!context.expired());
}

TEST_CASE(OperationContextReportsExpiry) {
    const auto deadline = std::chrono::steady_clock::now() -
                          std::chrono::milliseconds(10);
    const agent::OperationContext context{nullptr, deadline};
    REQUIRE(context.expired());
    REQUIRE(context.cancelled());
    REQUIRE(context.remaining_ms() == 0);
}

TEST_CASE(OperationContextRemainingTimeShrinks) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(150);
    const agent::OperationContext context{nullptr, deadline};
    const auto first = context.remaining_ms();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    const auto second = context.remaining_ms();
    REQUIRE(first > 0);
    REQUIRE(second >= 0);
    REQUIRE(second < first);
}

TEST_CASE(MakeOperationContextBuilderHonoursTimeout) {
    const auto context = agent::make_operation_context(
        nullptr, std::chrono::milliseconds(75));
    REQUIRE(context.remaining_ms() > 0);
    REQUIRE(context.remaining_ms() <= 75);
    REQUIRE(!context.expired());
}

TEST_CASE(OperationContextAllowsNullCancellation) {
    const agent::OperationContext context{nullptr,
        std::chrono::steady_clock::now() + std::chrono::seconds(60)};
    REQUIRE(!context.cancelled());
    REQUIRE(!context.expired());
}

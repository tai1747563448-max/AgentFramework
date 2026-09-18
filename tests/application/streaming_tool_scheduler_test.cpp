#include "application/streaming_tool_scheduler.h"
#include "adapters/empty/empty_tool_gateway.h"
#include "domain/model_types.h"
#include "test_support.h"

#include <chrono>
#include <optional>
#include <string>
#include <thread>

namespace {

TEST_CASE(streaming_scheduler_pre_dispatches_safe_tools) {
    struct SafeGateway final : agent::ToolGateway {
        bool tool_is_concurrency_safe(const std::string&) const override {
            return true;
        }
        std::vector<agent::ToolDefinition> definitions() const override {
            return {};
        }
        agent::Result<agent::ToolResult> execute(
            const agent::ToolCall& call,
            const agent::ToolExecutionContext&) override {
            return agent::Result<agent::ToolResult>::success(
                {call.id, "ok", false});
        }
    };
    SafeGateway gateway;
    agent::ToolCall call;
    call.id = "call-00112233445566778899aabbccddeeff";
    call.name = "list_files";
    call.arguments = agent::Value(std::string("/workspace"));
    std::vector<agent::ToolCall> calls{call};
    agent::ToolExecutionContext context{"/workspace"};
    auto scheduled = agent::schedule_pre_dispatch(calls, gateway, context);
    REQUIRE(scheduled.pre_dispatched.size() == 1u);
    REQUIRE(scheduled.pre_dispatched[0]);
    std::vector<std::optional<agent::Result<agent::ToolResult>>> results;
    agent::drain_pre_dispatch(scheduled, results);
    REQUIRE(results.size() == 1u);
    REQUIRE(results[0].has_value());
    REQUIRE(results[0]->has_value());
    REQUIRE(results[0]->value().tool_call_id == call.id);
}

TEST_CASE(streaming_scheduler_returns_nullopt_for_unsafe_tools) {
    agent::ToolGateway* raw = nullptr;
    // Use a tiny stub that reports the tool as NOT concurrency-safe
    // so the scheduler leaves the slot alone and drain leaves it
    // nullopt.
    struct StubGateway final : agent::ToolGateway {
        bool tool_is_concurrency_safe(const std::string&) const override {
            return false;
        }
        std::vector<agent::ToolDefinition> definitions() const override {
            return {};
        }
        agent::Result<agent::ToolResult> execute(
            const agent::ToolCall& call,
            const agent::ToolExecutionContext&) override {
            return agent::Result<agent::ToolResult>::success(
                {call.id, "", false});
        }
    };
    StubGateway stub;
    raw = &stub;
    (void)raw;
    agent::ToolCall call;
    call.id = "call-00112233445566778899aabbccddeeff";
    call.name = "replace_text";
    call.arguments = agent::Value(std::string(""));
    std::vector<agent::ToolCall> calls{call};
    agent::ToolExecutionContext context{"/workspace"};
    auto scheduled = agent::schedule_pre_dispatch(calls, stub, context);
    REQUIRE(scheduled.pre_dispatched.size() == 1u);
    REQUIRE(!scheduled.pre_dispatched[0]);
    std::vector<std::optional<agent::Result<agent::ToolResult>>> results;
    agent::drain_pre_dispatch(scheduled, results);
    REQUIRE(results.size() == 1u);
    REQUIRE(!results[0].has_value());
}

TEST_CASE(streaming_scheduler_handles_empty_call_list) {
    agent::EmptyToolGateway gateway;
    std::vector<agent::ToolCall> calls;
    agent::ToolExecutionContext context{"/workspace"};
    auto scheduled = agent::schedule_pre_dispatch(calls, gateway, context);
    REQUIRE(scheduled.pre_dispatched.empty());
    REQUIRE(scheduled.futures.empty());
}

}  // namespace
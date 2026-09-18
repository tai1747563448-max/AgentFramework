#include "application/hook_chain.h"

#include "domain/model_types.h"
#include "domain/task_state.h"

#include "test_support.h"

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using agent::ToolCall;
using agent::ToolResult;

TEST_CASE(test_audit_logger_records_tool_call) {
    agent::HookChain chain;
    chain.add_pre_tool_use(agent::make_audit_logger_hook());
    REQUIRE(chain.size() == 1);

    ToolCall call;
    call.id = "call-001";
    call.name = "list_files";
    agent::HookPreToolUse event{std::string{}, "task-test", &call, false, {}};
    chain.run_pre_tool_use(event);
    REQUIRE(!event.denied);
}

TEST_CASE(test_pre_tool_use_can_deny) {
    agent::HookChain chain;
    chain.add_pre_tool_use([](agent::HookPreToolUse& e) {
        e.denied = true;
        e.denial_reason = "policy violation";
    });
    ToolCall call{"c1", "write_file", {}};
    agent::HookPreToolUse event{"session-x", "task-x", &call, false, {}};
    chain.run_pre_tool_use(event);
    REQUIRE(event.denied);
    REQUIRE(event.denial_reason == "policy violation");
}

TEST_CASE(test_post_tool_use_receives_call_and_result) {
    agent::HookChain chain;
    std::string captured_name;
    std::string captured_content;
    bool captured_mutated = false;
    chain.add_post_tool_use([&](agent::HookPostToolUse& e) {
        captured_name = e.call.name;
        captured_content = e.result->content;
        e.mutated = true;
        captured_mutated = e.mutated;
    });
    ToolCall call{"c1", "read_file", {}};
    ToolResult result{"c1", "hello world", false};
    agent::HookPostToolUse event{"session", "task", call, &result, false};
    chain.run_post_tool_use(event);
    REQUIRE(captured_name == "read_file");
    REQUIRE(captured_content == "hello world");
    REQUIRE(captured_mutated);
}

TEST_CASE(test_throwing_hook_does_not_break_chain) {
    agent::HookChain chain;
    bool reached_second = false;
    chain.add_pre_tool_use([](agent::HookPreToolUse&) {
        throw std::runtime_error("misbehaving hook");
    });
    chain.add_pre_tool_use([&](agent::HookPreToolUse&) {
        reached_second = true;
    });
    ToolCall call{"c1", "list_files", {}};
    agent::HookPreToolUse event{"", "task", &call, false, {}};
    chain.run_pre_tool_use(event);
    REQUIRE(reached_second);
}

TEST_CASE(test_chain_size_counts_all_four_legs) {
    agent::HookChain chain;
    REQUIRE(chain.size() == 0);
    chain.add_pre_model_call([](agent::HookPreModelCall&) {});
    chain.add_post_model_call([](agent::HookPostModelCall&) {});
    chain.add_pre_tool_use([](agent::HookPreToolUse&) {});
    chain.add_post_tool_use([](agent::HookPostToolUse&) {});
    REQUIRE(chain.size() == 4);
}
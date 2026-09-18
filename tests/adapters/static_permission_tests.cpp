#include "adapters/permission/static_permission.h"
#include "ports/permission.h"
#include "ports/tool_gateway.h"
#include "test_support.h"

#include <string>

using agent::PermissionDecision;
using agent::PermissionMode;
using agent::parse_permission_mode;
using agent::permission_decision_name;
using agent::permission_mode_name;
using agent::StaticPermission;

namespace {

agent::ToolCall make_call(const std::string& name) {
    return {std::string("call-1"), name,
            agent::Value::object({{"path", agent::Value("src/main.cpp")}})};
}

agent::ToolExecutionContext make_context() { return {"/workspace"}; }

}  // namespace

TEST_CASE(parse_permission_mode_recognises_every_documented_value) {
    REQUIRE(parse_permission_mode("default").has_value());
    REQUIRE(*parse_permission_mode("default") == PermissionMode::Default);
    REQUIRE(*parse_permission_mode("acceptEdits") ==
            PermissionMode::AcceptEdits);
    REQUIRE(*parse_permission_mode("plan") == PermissionMode::Plan);
    REQUIRE(*parse_permission_mode("bypassPermissions") ==
            PermissionMode::BypassPermissions);
    REQUIRE(*parse_permission_mode("auto") == PermissionMode::Auto);
    REQUIRE(!parse_permission_mode("nonsense").has_value());
}

TEST_CASE(permission_mode_name_is_stable_for_every_value) {
    REQUIRE(std::string(permission_mode_name(PermissionMode::Default)) ==
            "default");
    REQUIRE(std::string(permission_mode_name(PermissionMode::AcceptEdits)) ==
            "acceptEdits");
    REQUIRE(std::string(permission_mode_name(PermissionMode::Plan)) ==
            "plan");
    REQUIRE(std::string(permission_mode_name(
                PermissionMode::BypassPermissions)) == "bypassPermissions");
    REQUIRE(std::string(permission_mode_name(PermissionMode::Auto)) ==
            "auto");
}

TEST_CASE(default_mode_allows_read_tools_and_asks_for_writes) {
    StaticPermission permission;
    REQUIRE(permission.check(make_call("list_files"), make_context()) ==
            PermissionDecision::Allow);
    REQUIRE(permission.check(make_call("read_file"), make_context()) ==
            PermissionDecision::Allow);
    REQUIRE(permission.check(make_call("search_text"), make_context()) ==
            PermissionDecision::Allow);
    REQUIRE(permission.check(make_call("replace_text"), make_context()) ==
            PermissionDecision::Ask);
    REQUIRE(permission.check(make_call("write_file"), make_context()) ==
            PermissionDecision::Ask);
}

TEST_CASE(accept_edits_mode_allows_workspace_writes_silently) {
    StaticPermission permission(PermissionMode::AcceptEdits);
    REQUIRE(permission.check(make_call("replace_text"), make_context()) ==
            PermissionDecision::Allow);
    REQUIRE(permission.check(make_call("write_file"), make_context()) ==
            PermissionDecision::Allow);
    REQUIRE(permission.check(make_call("shell_exec"), make_context()) ==
            PermissionDecision::Ask);
}

TEST_CASE(plan_mode_denies_every_tool) {
    StaticPermission permission(PermissionMode::Plan);
    REQUIRE(permission.check(make_call("read_file"), make_context()) ==
            PermissionDecision::Deny);
    REQUIRE(permission.check(make_call("write_file"), make_context()) ==
            PermissionDecision::Deny);
}

TEST_CASE(bypass_permissions_mode_allows_every_tool) {
    StaticPermission permission(PermissionMode::BypassPermissions);
    REQUIRE(permission.check(make_call("read_file"), make_context()) ==
            PermissionDecision::Allow);
    REQUIRE(permission.check(make_call("write_file"), make_context()) ==
            PermissionDecision::Allow);
    REQUIRE(permission.check(make_call("shell_exec"), make_context()) ==
            PermissionDecision::Allow);
}

TEST_CASE(explicit_rules_override_mode_default) {
    StaticPermission permission(PermissionMode::BypassPermissions);
    permission.set_rule("read_file", PermissionDecision::Deny);
    REQUIRE(permission.check(make_call("read_file"), make_context()) ==
            PermissionDecision::Deny);
    REQUIRE(permission.check(make_call("write_file"), make_context()) ==
            PermissionDecision::Allow);
}

TEST_CASE(decision_name_is_stable_for_every_value) {
    REQUIRE(std::string(permission_decision_name(PermissionDecision::Allow)) ==
            "allow");
    REQUIRE(std::string(permission_decision_name(PermissionDecision::Deny)) ==
            "deny");
    REQUIRE(std::string(permission_decision_name(PermissionDecision::Ask)) ==
            "ask");
}

TEST_CASE(serialise_permission_state_round_trip_is_human_readable) {
    StaticPermission permission(PermissionMode::AcceptEdits);
    permission.set_rule("replace_text", PermissionDecision::Allow);
    const auto text = agent::serialise_permission_state(permission);
    REQUIRE(text.find("\"mode\":\"acceptEdits\"") != std::string::npos);
    REQUIRE(text.find("\"tool\":\"replace_text\"") != std::string::npos);
    REQUIRE(text.find("\"decision\":\"allow\"") != std::string::npos);
}

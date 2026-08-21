#include "adapters/tools/composite_tool_gateway.h"
#include "ports/tool_gateway.h"
#include "test_support.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fixtures {

agent::ToolDefinition definition(std::string name) {
    return {std::move(name), "test definition",
            agent::Value::object({{"type", "object"},
                                  {"properties", agent::Value::object({})},
                                  {"required", agent::Value::array({})},
                                  {"additionalProperties", false}})};
}

class FakeGateway final : public agent::ToolGateway {
public:
    FakeGateway(std::string result_content,
                std::vector<agent::ToolDefinition> definitions)
        : result_content_(std::move(result_content)),
          definitions_(std::move(definitions)) {}

    std::vector<agent::ToolDefinition> definitions() const override {
        return definitions_;
    }

    agent::Result<agent::ToolResult> execute(
        const agent::ToolCall& call,
        const agent::ToolExecutionContext& context) override {
        ++calls;
        observed_name = call.name;
        observed_workspace = context.workspace_utf8;
        return agent::Result<agent::ToolResult>::success(
            {call.id, result_content_, false});
    }

    int calls{0};
    std::string observed_name;
    std::string observed_workspace;

private:
    std::string result_content_;
    std::vector<agent::ToolDefinition> definitions_;
};

}  // namespace fixtures

TEST_CASE(composite_gateway_preserves_definition_order_and_routes_exactly) {
    fixtures::FakeGateway first(
        "first", {fixtures::definition("list_files"),
                  fixtures::definition("read_file")});
    fixtures::FakeGateway second(
        "second", {fixtures::definition("configure_project"),
                   fixtures::definition("run_tests")});
    agent::CompositeToolGateway composite(
        {std::ref(first), std::ref(second)});

    const auto definitions = composite.definitions();
    REQUIRE(definitions.size() == 4);
    REQUIRE(definitions.at(0).name == "list_files");
    REQUIRE(definitions.at(1).name == "read_file");
    REQUIRE(definitions.at(2).name == "configure_project");
    REQUIRE(definitions.at(3).name == "run_tests");

    const auto result = composite.execute(
        {"call-1", "run_tests", agent::Value::object({})},
        {u8"E:/工作区🙂"});
    REQUIRE(result.has_value());
    REQUIRE(result.value().content == "second");
    REQUIRE(result.value().tool_call_id == "call-1");
    REQUIRE(first.calls == 0);
    REQUIRE(second.calls == 1);
    REQUIRE(second.observed_name == "run_tests");
    REQUIRE(second.observed_workspace == u8"E:/工作区🙂");
}

TEST_CASE(composite_gateway_rejects_duplicate_definition_names) {
    fixtures::FakeGateway first("first", {fixtures::definition("same")});
    fixtures::FakeGateway second("second", {fixtures::definition("same")});
    bool rejected = false;
    try {
        agent::CompositeToolGateway composite(
            {std::ref(first), std::ref(second)});
        (void)composite;
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    REQUIRE(rejected);
}

TEST_CASE(composite_gateway_unknown_tool_is_bounded_and_calls_no_child) {
    fixtures::FakeGateway first("first", {fixtures::definition("known")});
    agent::CompositeToolGateway composite({std::ref(first)});

    const auto result = composite.execute(
        {"call-unknown", "run_shell", agent::Value::object({})},
        {"E:/workspace"});
    REQUIRE(result.has_value());
    REQUIRE(result.value().tool_call_id == "call-unknown");
    REQUIRE(result.value().is_error);
    const auto json = nlohmann::json::parse(result.value().content);
    REQUIRE(json.at("error").at("code") == "invalid_arguments");
    REQUIRE(json.at("error").at("message") == "unknown tool");
    REQUIRE(json.at("error").at("retryable") == false);
    REQUIRE(result.value().content.size() < 256);
    REQUIRE(first.calls == 0);
}

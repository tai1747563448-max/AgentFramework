#include "adapters/build/cmake_tool_gateway.h"
#include "ports/process_runner.h"
#include "ports/tool_gateway.h"
#include "test_support.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fixtures {

class FakeProcessRunner final : public agent::ProcessRunner {
public:
    agent::Result<agent::ProcessOutput> run(
        const agent::ProcessRequest& request) override {
        requests.push_back(request);
        if (failure.has_value()) {
            return agent::Result<agent::ProcessOutput>::failure(*failure);
        }
        return agent::Result<agent::ProcessOutput>::success(output);
    }

    std::vector<agent::ProcessRequest> requests;
    agent::ProcessOutput output{0, false, 17, "stdout", "stderr", false,
                                false};
    std::optional<agent::RuntimeError> failure;
};

agent::ToolCall call(std::string id,
                     std::string name,
                     agent::Value::Object arguments) {
    return {std::move(id), std::move(name),
            agent::Value::object(std::move(arguments))};
}

nlohmann::json content_json(const agent::Result<agent::ToolResult>& result) {
    REQUIRE(result.has_value());
    return nlohmann::json::parse(result.value().content);
}

std::string error_code(const agent::Result<agent::ToolResult>& result) {
    REQUIRE(result.has_value());
    REQUIRE(result.value().is_error);
    return content_json(result).at("error").at("code").get<std::string>();
}

agent::Value object_schema(agent::Value::Object properties,
                           agent::Value::Array required) {
    return agent::Value::object({
        {"type", "object"},
        {"properties", agent::Value::object(std::move(properties))},
        {"required", agent::Value::array(std::move(required))},
        {"additionalProperties", false}});
}

agent::Value configuration_schema() {
    return agent::Value::object(
        {{"type", "string"},
         {"enum", agent::Value::array({"Debug", "Release"})}});
}

void require_common_request(const agent::ProcessRequest& request,
                            const std::filesystem::path& workspace) {
    REQUIRE(request.working_directory ==
            std::filesystem::weakly_canonical(workspace));
    REQUIRE(request.stdin_utf8.empty());
    REQUIRE(request.timeout_ms == 123'000);
    REQUIRE(request.max_stdout_bytes == 64 * 1024);
    REQUIRE(request.max_stderr_bytes == 64 * 1024);
    REQUIRE((request.environment_overrides ==
             std::vector<std::pair<std::string, std::string>>(
                 {{"VSLANG", "1033"}, {"CLICOLOR", "0"}})));
}

}  // namespace fixtures

TEST_CASE(cmake_gateway_exposes_exact_three_closed_schemas) {
    fixtures::FakeProcessRunner process;
    agent::CMakeToolGateway gateway(process, 123'000);
    const auto definitions = gateway.definitions();
    const auto string_schema = agent::Value::object({{"type", "string"}});
    const std::vector<agent::ToolDefinition> expected{
        {"configure_project", "Configure the trusted workspace with CMake.",
         fixtures::object_schema(
             {{"configuration", fixtures::configuration_schema()}},
             {"configuration"})},
        {"build_project", "Build the configured CMake workspace.",
         fixtures::object_schema(
             {{"configuration", fixtures::configuration_schema()},
              {"target", string_schema}},
             {"configuration"})},
        {"run_tests", "Run CTest in the configured workspace.",
         fixtures::object_schema(
             {{"configuration", fixtures::configuration_schema()},
              {"test_name", string_schema}},
             {"configuration"})}};
    REQUIRE(definitions == expected);
}

TEST_CASE(cmake_gateway_emits_only_fixed_programs_and_arguments) {
    test::ScopedTempDir temp(std::filesystem::u8path(u8"构建网关-🙂"));
    fixtures::FakeProcessRunner process;
    agent::CMakeToolGateway gateway(process, 123'000);
    const agent::ToolExecutionContext context{temp.path().generic_u8string()};
    const auto root = std::filesystem::weakly_canonical(temp.path());
    const auto build = root / ".agent/cmake-build";

    auto result = gateway.execute(
        fixtures::call("configure", "configure_project",
                       {{"configuration", "Debug"}}),
        context);
    REQUIRE(result.has_value() && !result.value().is_error);
    REQUIRE(process.requests.size() == 1);
    REQUIRE(process.requests.back().program == "cmake");
    REQUIRE(process.requests.back().arguments ==
            std::vector<std::string>(
                {"-S", root.generic_u8string(), "-B",
                 build.generic_u8string(), "-DCMAKE_BUILD_TYPE=Debug"}));
    fixtures::require_common_request(process.requests.back(), root);

    result = gateway.execute(
        fixtures::call("configure-release", "configure_project",
                       {{"configuration", "Release"}}),
        context);
    REQUIRE(result.has_value() && !result.value().is_error);
    REQUIRE(process.requests.back().arguments ==
            std::vector<std::string>(
                {"-S", root.generic_u8string(), "-B",
                 build.generic_u8string(), "-DCMAKE_BUILD_TYPE=Release"}));
    fixtures::require_common_request(process.requests.back(), root);

    result = gateway.execute(
        fixtures::call("build-all", "build_project",
                       {{"configuration", "Debug"}}),
        context);
    REQUIRE(result.has_value() && !result.value().is_error);
    REQUIRE(process.requests.back().arguments ==
            std::vector<std::string>(
                {"--build", build.generic_u8string(), "--config", "Debug"}));
    fixtures::require_common_request(process.requests.back(), root);

    result = gateway.execute(
        fixtures::call("build", "build_project",
                       {{"configuration", "Release"},
                        {"target", "runtime_engine_tests"}}),
        context);
    REQUIRE(result.has_value() && !result.value().is_error);
    REQUIRE(process.requests.back().program == "cmake");
    REQUIRE(process.requests.back().arguments ==
            std::vector<std::string>(
                {"--build", build.generic_u8string(), "--config", "Release",
                 "--target", "runtime_engine_tests"}));
    fixtures::require_common_request(process.requests.back(), root);

    result = gateway.execute(
        fixtures::call("all-tests", "run_tests",
                       {{"configuration", "Release"}}),
        context);
    REQUIRE(result.has_value() && !result.value().is_error);
    REQUIRE(process.requests.back().arguments ==
            std::vector<std::string>(
                {"--test-dir", build.generic_u8string(), "-C", "Release",
                 "--output-on-failure", "--no-tests=error"}));
    fixtures::require_common_request(process.requests.back(), root);

    result = gateway.execute(
        fixtures::call("tests", "run_tests",
                       {{"configuration", "Debug"},
                        {"test_name", "suite[1].cpp"}}),
        context);
    REQUIRE(result.has_value() && !result.value().is_error);
    REQUIRE(process.requests.back().program == "ctest");
    REQUIRE(process.requests.back().arguments ==
            std::vector<std::string>(
                {"--test-dir", build.generic_u8string(), "-C", "Debug",
                 "--output-on-failure", "--no-tests=error", "-R",
                 "^suite\\[1\\]\\.cpp$"}));
    fixtures::require_common_request(process.requests.back(), root);

    const auto json = fixtures::content_json(result);
    REQUIRE(json.at("operation") == "test");
    REQUIRE(json.at("exit_code") == 0);
    REQUIRE(json.at("timed_out") == false);
    REQUIRE(json.at("duration_ms") == 17);
    REQUIRE(json.at("stdout") == "stdout");
    REQUIRE(json.at("stderr") == "stderr");
    REQUIRE(json.at("stdout_truncated") == false);
    REQUIRE(json.at("stderr_truncated") == false);
}

TEST_CASE(cmake_gateway_rejects_open_ended_or_unsafe_inputs_before_process) {
    test::ScopedTempDir temp("cmake-invalid");
    fixtures::FakeProcessRunner process;
    agent::CMakeToolGateway gateway(process, 123'000);
    const agent::ToolExecutionContext context{temp.path().generic_u8string()};
    const std::string invalid_utf8(1, static_cast<char>(0xFF));

    const std::vector<agent::ToolCall> invalid{
        fixtures::call("bad-config", "configure_project",
                       {{"configuration", "RelWithDebInfo"}}),
        fixtures::call("extra", "configure_project",
                       {{"configuration", "Debug"}, {"flag", "-G"}}),
        fixtures::call("bad-target", "build_project",
                       {{"configuration", "Debug"},
                        {"target", "all -- /p:owned"}}),
        fixtures::call("empty-target", "build_project",
                       {{"configuration", "Debug"}, {"target", ""}}),
        fixtures::call("empty-test", "run_tests",
                       {{"configuration", "Debug"}, {"test_name", ""}}),
        fixtures::call("invalid-test", "run_tests",
                       {{"configuration", "Debug"},
                        {"test_name", invalid_utf8}}),
        fixtures::call("unknown", "run_shell", {})};
    for (const auto& call : invalid) {
        REQUIRE(fixtures::error_code(gateway.execute(call, context)) ==
                "invalid_arguments");
    }
    REQUIRE(process.requests.empty());

    const agent::ToolExecutionContext relative_context{"."};
    REQUIRE(fixtures::error_code(gateway.execute(
                fixtures::call("relative", "configure_project",
                               {{"configuration", "Debug"}}),
                relative_context)) == "access_denied");
    REQUIRE(process.requests.empty());

    temp.write_text(".agent", "not a directory");
    REQUIRE(fixtures::error_code(gateway.execute(
                fixtures::call("unsafe", "configure_project",
                               {{"configuration", "Debug"}}),
                context)) == "access_denied");
    REQUIRE(process.requests.empty());
}

TEST_CASE(cmake_gateway_returns_bounded_process_failure_evidence) {
    test::ScopedTempDir temp("cmake-results");
    fixtures::FakeProcessRunner process;
    agent::CMakeToolGateway gateway(process, 123'000);
    const agent::ToolExecutionContext context{temp.path().generic_u8string()};
    const auto build_call = fixtures::call(
        "build", "build_project", {{"configuration", "Debug"}});

    process.output = {5, false, 91, "compile output", "compile error", false,
                      false};
    auto result = gateway.execute(build_call, context);
    REQUIRE(result.has_value() && result.value().is_error);
    auto json = fixtures::content_json(result);
    REQUIRE(json.at("operation") == "build");
    REQUIRE(json.at("exit_code") == 5);
    REQUIRE(json.at("stderr") == "compile error");

    process.output = {-1, true, 123'000, "", "", false, false};
    result = gateway.execute(build_call, context);
    REQUIRE(result.has_value() && result.value().is_error);
    json = fixtures::content_json(result);
    REQUIRE(json.at("timed_out") == true);
    REQUIRE(json.at("exit_code") == -1);

    const nlohmann::json empty_process_json{
        {"operation", "build"},
        {"exit_code", 0},
        {"timed_out", false},
        {"duration_ms", 2},
        {"stdout", ""},
        {"stderr", ""},
        {"stdout_truncated", true},
        {"stderr_truncated", true}};
    process.output = {
        0, false, 2,
        std::string(65'536 - empty_process_json.dump().size(), 'x'),
        "", false, false};
    result = gateway.execute(build_call, context);
    REQUIRE(result.has_value() && !result.value().is_error);
    REQUIRE(result.value().content.size() <= 65'536);
    json = fixtures::content_json(result);
    REQUIRE(json.at("operation") == "build");
    REQUIRE(json.at("stdout_truncated") == true);

    process.output = {0, false, 2,
                      std::string(60'000, static_cast<char>(0x01)),
                      std::string(60'000, static_cast<char>(0x02)), false,
                      false};
    result = gateway.execute(build_call, context);
    REQUIRE(result.has_value() && !result.value().is_error);
    REQUIRE(result.value().content.size() <= 65'536);
    json = fixtures::content_json(result);
    REQUIRE(json.at("stdout_truncated") == true);
    REQUIRE(json.at("stderr_truncated") == true);

    process.failure = agent::RuntimeError{
        agent::ErrorCode::DependencyUnavailable, "SENTINEL_OS_ERROR", false};
    result = gateway.execute(build_call, context);
    REQUIRE(fixtures::error_code(result) == "process_unavailable");
    REQUIRE(result.value().content.find("SENTINEL_OS_ERROR") ==
            std::string::npos);
}

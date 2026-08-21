#include "adapters/process/direct_process_runner.h"
#include "ports/process_runner.h"
#include "test_support.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fixtures {

std::string executable() {
    return AGENT_PROCESS_FIXTURE_PATH;
}

class ScopedEnvironment final {
public:
    ScopedEnvironment(std::string name, std::string value)
        : name_(std::move(name)) {
        const char* prior = std::getenv(name_.c_str());
        if (prior != nullptr) {
            prior_ = prior;
        }
#if defined(_WIN32)
        _putenv_s(name_.c_str(), value.c_str());
#else
        setenv(name_.c_str(), value.c_str(), 1);
#endif
    }

    ~ScopedEnvironment() {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), prior_.has_value() ? prior_->c_str() : "");
#else
        if (prior_.has_value()) {
            setenv(name_.c_str(), prior_->c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
#endif
    }

private:
    std::string name_;
    std::optional<std::string> prior_;
};

agent::ProcessRequest request(const std::filesystem::path& cwd,
                              std::vector<std::string> arguments,
                              std::string stdin_utf8 = {}) {
    return {executable(), std::move(arguments), cwd, std::move(stdin_utf8),
            5'000, 64 * 1024, 64 * 1024, {{"VSLANG", "1033"}}};
}

}  // namespace fixtures

TEST_CASE(direct_process_preserves_literal_unicode_arguments_cwd_and_stdin) {
    test::ScopedTempDir temp(std::filesystem::u8path(u8"进程-🙂"));
    fixtures::ScopedEnvironment api_key("AGENT_API_KEY", "SENTINEL_KEY");
    fixtures::ScopedEnvironment auth_token("AGENT_AUTH_TOKEN", "SENTINEL_TOKEN");
    fixtures::ScopedEnvironment secret("SAMPLE_SECRET", "SENTINEL_SECRET");
    agent::DirectProcessRunner runner;
    const auto sentinel = temp.path() / "must-not-exist.txt";
    auto process = fixtures::request(
        temp.path(), {"inspect", u8"参数🙂", "&|><\"", sentinel.generic_u8string()},
        u8"标准输入🙂");
    process.environment_overrides.emplace_back("AGENT_API_KEY", "OVERRIDE_KEY");
    process.environment_overrides.emplace_back("AGENT_AUTH_TOKEN",
                                                "OVERRIDE_TOKEN");
    process.environment_overrides.emplace_back("SAMPLE_SECRET",
                                                "OVERRIDE_SECRET");

    const auto result = runner.run(process);
    REQUIRE(result.has_value());
    if (result.value().exit_code != 0) {
        throw std::runtime_error(
            "fixture exit=" + std::to_string(result.value().exit_code) +
            " stderr=" + result.value().stderr_utf8);
    }
    REQUIRE(!result.value().timed_out);
    REQUIRE(result.value().stderr_utf8.empty());
    const auto json = nlohmann::json::parse(result.value().stdout_utf8);
    REQUIRE(json.at("cwd") == temp.path().generic_u8string());
    REQUIRE(json.at("arguments") ==
            nlohmann::json::array({u8"参数🙂", "&|><\"",
                                   sentinel.generic_u8string()}));
    REQUIRE(json.at("stdin") == u8"标准输入🙂");
    REQUIRE(json.at("environment").at("AGENT_API_KEY") == false);
    REQUIRE(json.at("environment").at("AGENT_AUTH_TOKEN") == false);
    REQUIRE(json.at("environment").at("SAMPLE_SECRET") == false);
    REQUIRE(json.at("environment").at("PATH") == true);
    REQUIRE(!std::filesystem::exists(sentinel));
}

TEST_CASE(direct_process_bounds_concurrent_stdout_and_stderr) {
    test::ScopedTempDir temp("process-output");
    agent::DirectProcessRunner runner;
    auto process = fixtures::request(temp.path(), {"spam", "200000", "200000", "7"});
    process.max_stdout_bytes = 257;
    process.max_stderr_bytes = 193;

    const auto result = runner.run(process);
    REQUIRE(result.has_value());
    REQUIRE(result.value().exit_code == 7);
    REQUIRE(!result.value().timed_out);
    REQUIRE(result.value().stdout_truncated);
    REQUIRE(result.value().stderr_truncated);
    REQUIRE(result.value().stdout_utf8.size() <= 257);
    REQUIRE(result.value().stderr_utf8.size() <= 193);
    REQUIRE(result.value().stdout_utf8.front() == 'H');
    REQUIRE(result.value().stdout_utf8.back() == 'T');
    REQUIRE(result.value().stderr_utf8.front() == 'H');
    REQUIRE(result.value().stderr_utf8.back() == 'T');
}

TEST_CASE(direct_process_timeout_kills_the_descendant_tree) {
    test::ScopedTempDir temp("process-timeout");
    agent::DirectProcessRunner runner;
    const auto marker = temp.path() / "descendant-marker.txt";
    auto process = fixtures::request(
        temp.path(), {"tree-parent", marker.generic_u8string(), "500"});
    process.timeout_ms = 100;
    const auto started = std::chrono::steady_clock::now();

    const auto result = runner.run(process);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    REQUIRE(result.has_value());
    REQUIRE(result.value().timed_out);
    REQUIRE(elapsed < std::chrono::seconds(5));
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    REQUIRE(!std::filesystem::exists(marker));
}

TEST_CASE(direct_process_missing_program_returns_a_fixed_error) {
    test::ScopedTempDir temp("process-missing");
    agent::DirectProcessRunner runner;
    auto process = fixtures::request(temp.path(), {});
    process.program = "agent-definitely-missing-program-3f91f8";

    const auto result = runner.run(process);
    REQUIRE(!result.has_value());
    REQUIRE(result.error().code == agent::ErrorCode::DependencyUnavailable);
    REQUIRE(result.error().message == "process could not be started");
    REQUIRE(!result.error().retryable);
}

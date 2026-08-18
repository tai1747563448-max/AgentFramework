#include "adapters/empty/empty_knowledge_provider.h"
#include "adapters/empty/empty_tool_gateway.h"
#include "adapters/system/random_id_generator.h"
#include "adapters/system/signal_cancellation.h"
#include "adapters/system/system_clock.h"
#include "cli/cli_app.h"
#include "config/runtime_config.h"
#include "test_support.h"

#include <algorithm>
#include <cstdint>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace test {

class MapEnvironment final : public agent::Environment {
public:
    MapEnvironment(
        std::initializer_list<std::pair<const std::string, std::string>> values)
        : values_(values) {}

    std::optional<std::string> get(const std::string& name) const override {
        const auto found = values_.find(name);
        if (found == values_.end()) {
            return std::nullopt;
        }
        return found->second;
    }

private:
    std::map<std::string, std::string> values_;
};

#if defined(_WIN32)
std::optional<std::wstring> wide_environment_value(const wchar_t* name) {
    SetLastError(ERROR_SUCCESS);
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) {
        if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
            return std::nullopt;
        }
        return std::wstring{};
    }
    std::wstring value(required, L'\0');
    const DWORD written =
        GetEnvironmentVariableW(name, value.data(), required);
    if (written >= required) {
        return std::nullopt;
    }
    value.resize(written);
    return value;
}

class ScopedWideEnvironment final {
public:
    ScopedWideEnvironment(std::wstring name, std::wstring value)
        : name_(std::move(name)), previous_(wide_environment_value(name_.c_str())) {
        REQUIRE(SetEnvironmentVariableW(name_.c_str(), value.c_str()) != FALSE);
    }

    ~ScopedWideEnvironment() {
        SetEnvironmentVariableW(
            name_.c_str(), previous_.has_value() ? previous_->c_str() : nullptr);
    }

    ScopedWideEnvironment(const ScopedWideEnvironment&) = delete;
    ScopedWideEnvironment& operator=(const ScopedWideEnvironment&) = delete;

private:
    std::wstring name_;
    std::optional<std::wstring> previous_;
};
#endif

agent::TaskState terminal_state(agent::TaskStatus status) {
    agent::TaskState state;
    state.task_id = "task-cli";
    state.status = status;
    state.last_sequence = 9;
    if (status == agent::TaskStatus::Completed) {
        state.final_text = "done";
    } else {
        state.terminal_error = agent::RuntimeError{
            status == agent::TaskStatus::BudgetExceeded
                ? agent::ErrorCode::BudgetExceeded
                : status == agent::TaskStatus::Cancelled
                      ? agent::ErrorCode::Cancelled
                      : agent::ErrorCode::ProtocolFailure,
            "SENTINEL_PRIVATE_FAILURE_DETAIL", false};
    }
    return state;
}

agent::RuntimeResult terminal_result(agent::TaskStatus status) {
    return {terminal_state(status), std::nullopt};
}

agent::Result<agent::TaskState> verified_state(agent::TaskStatus status) {
    return agent::Result<agent::TaskState>::success(terminal_state(status));
}

}  // namespace test

namespace fixtures {

test::MapEnvironment valid_environment() {
    return {{"AGENT_BASE_URL", "https://provider.example"},
            {"AGENT_MODEL", "model-id"},
            {"AGENT_API_KEY", "test-api-key"}};
}

}  // namespace fixtures

TEST_CASE(cli_exit_codes_are_stable) {
    REQUIRE(static_cast<int>(agent::ExitCode::Success) == 0);
    REQUIRE(static_cast<int>(agent::ExitCode::InvalidInputOrConfig) == 2);
    REQUIRE(static_cast<int>(agent::ExitCode::TaskFailed) == 3);
    REQUIRE(static_cast<int>(agent::ExitCode::BudgetExceeded) == 4);
    REQUIRE(static_cast<int>(agent::ExitCode::Cancelled) == 5);
    REQUIRE(static_cast<int>(agent::ExitCode::PersistenceFailure) == 6);
    REQUIRE(static_cast<int>(agent::ExitCode::InvalidEventLog) == 7);
}

TEST_CASE(cli_maps_completed_task_to_zero_and_forwards_user_input) {
    std::vector<agent::RunRequest> requests;
    agent::RunCommand run = [&](const agent::RunRequest& request) {
        requests.push_back(request);
        return test::terminal_result(agent::TaskStatus::Completed);
    };
    agent::VerifyCommand verify = [](const std::filesystem::path&) {
        return test::verified_state(agent::TaskStatus::Completed);
    };
    std::ostringstream out;
    std::ostringstream err;
    agent::CliApp app(run, verify, out, err);

    const int code = app.execute(
        {"agent", "run", "--workspace", u8"E:/工作区", "--issue", u8"修复警告"});

    REQUIRE(code == agent::ExitCode::Success);
    REQUIRE(requests.size() == 1);
    REQUIRE(requests.front().workspace_utf8 == u8"E:/工作区");
    REQUIRE(requests.front().issue == u8"修复警告");
    REQUIRE(out.str().find("done") != std::string::npos);
    REQUIRE(err.str().empty());
}

TEST_CASE(cli_sanitizes_controls_invalid_utf8_and_embedded_nul) {
    std::string unsafe = "safe";
    unsafe.push_back('\x1b');
    unsafe += "[31m";
    unsafe.push_back('\0');
    unsafe += " red ";
    unsafe.push_back(static_cast<char>(0xF0));
    unsafe.push_back('(');
    unsafe.push_back(static_cast<char>(0x8C));
    unsafe.push_back('(');
    auto completed = test::terminal_result(agent::TaskStatus::Completed);
    completed.state->final_text = unsafe;
    std::ostringstream out;
    std::ostringstream err;
    agent::CliApp app(
        [completed](const agent::RunRequest&) { return completed; },
        [](const std::filesystem::path&) {
            return test::verified_state(agent::TaskStatus::Completed);
        },
        out, err);

    const auto code = app.execute(
        {"agent", "run", "--workspace", ".", "--issue", "test"});

    REQUIRE(code == agent::ExitCode::Success);
    REQUIRE(out.str() ==
            "safe\\u001B[31m\\u0000 red \\xF0(\\x8C(\n");
    REQUIRE(err.str().empty());
}

TEST_CASE(cli_preserves_printable_unicode_newline_and_tab) {
    auto completed = test::terminal_result(agent::TaskStatus::Completed);
    completed.state->final_text = u8"中文🙂\n\t完成";
    std::ostringstream out;
    std::ostringstream err;
    agent::CliApp app(
        [completed](const agent::RunRequest&) { return completed; },
        [](const std::filesystem::path&) {
            return test::verified_state(agent::TaskStatus::Completed);
        },
        out, err);

    REQUIRE(app.execute(
                {"agent", "run", "--workspace", ".", "--issue", "test"}) ==
            agent::ExitCode::Success);
    REQUIRE(out.str() == std::string{u8"中文🙂\n\t完成\n"});
    REQUIRE(err.str().empty());
}

TEST_CASE(cli_truncates_at_a_utf8_boundary_after_at_most_8192_rendered_bytes) {
    std::string oversized(8191, 'a');
    oversized += u8"中";
    oversized += std::string(100, 'b');
    auto completed = test::terminal_result(agent::TaskStatus::Completed);
    completed.state->final_text = std::move(oversized);
    std::ostringstream out;
    std::ostringstream err;
    agent::CliApp app(
        [completed](const agent::RunRequest&) { return completed; },
        [](const std::filesystem::path&) {
            return test::verified_state(agent::TaskStatus::Completed);
        },
        out, err);

    REQUIRE(app.execute(
                {"agent", "run", "--workspace", ".", "--issue", "test"}) ==
            agent::ExitCode::Success);
    const auto marker = out.str().find("\n[output truncated]");
    REQUIRE(marker != std::string::npos);
    REQUIRE(marker <= 8192);
    REQUIRE(marker == 8191);
    REQUIRE(out.str().substr(0, marker) == std::string(8191, 'a'));
    REQUIRE(out.str().find(u8"中") == std::string::npos);
    REQUIRE(err.str().empty());
}

TEST_CASE(cli_maps_terminal_statuses_without_printing_terminal_details) {
    const std::vector<std::pair<agent::TaskStatus, int>> cases = {
        {agent::TaskStatus::Failed, agent::ExitCode::TaskFailed},
        {agent::TaskStatus::BudgetExceeded, agent::ExitCode::BudgetExceeded},
        {agent::TaskStatus::Cancelled, agent::ExitCode::Cancelled},
    };
    for (const auto& item : cases) {
        std::ostringstream out;
        std::ostringstream err;
        agent::CliApp app(
            [status = item.first](const agent::RunRequest&) {
                return test::terminal_result(status);
            },
            [](const std::filesystem::path&) {
                return test::verified_state(agent::TaskStatus::Completed);
            },
            out, err);

        REQUIRE(app.execute({"agent", "run", "--workspace", ".", "--issue",
                             "test"}) == item.second);
        REQUIRE(out.str().find("SENTINEL_PRIVATE_FAILURE_DETAIL") ==
                std::string::npos);
        REQUIRE(err.str().find("SENTINEL_PRIVATE_FAILURE_DETAIL") ==
                std::string::npos);
    }
}

TEST_CASE(cli_maps_fatal_errors_to_stable_exit_codes) {
    const std::vector<std::pair<agent::ErrorCode, int>> cases = {
        {agent::ErrorCode::InvalidInput, agent::ExitCode::InvalidInputOrConfig},
        {agent::ErrorCode::InvalidConfiguration,
         agent::ExitCode::InvalidInputOrConfig},
        {agent::ErrorCode::PersistenceFailure,
         agent::ExitCode::PersistenceFailure},
        {agent::ErrorCode::BudgetExceeded, agent::ExitCode::BudgetExceeded},
        {agent::ErrorCode::Cancelled, agent::ExitCode::Cancelled},
        {agent::ErrorCode::TransportFailure, agent::ExitCode::TaskFailed},
    };
    for (const auto& item : cases) {
        std::ostringstream out;
        std::ostringstream err;
        agent::CliApp app(
            [code = item.first](const agent::RunRequest&) {
                return agent::RuntimeResult{
                    std::nullopt,
                    agent::RuntimeError{code, "SENTINEL_FATAL_DETAIL", false}};
            },
            [](const std::filesystem::path&) {
                return test::verified_state(agent::TaskStatus::Completed);
            },
            out, err);
        REQUIRE(app.execute({"agent", "run", "--workspace", ".", "--issue",
                             "test"}) == item.second);
        REQUIRE(out.str().find("SENTINEL_FATAL_DETAIL") == std::string::npos);
        REQUIRE(err.str().find("SENTINEL_FATAL_DETAIL") == std::string::npos);
    }
}

TEST_CASE(cli_rejects_missing_duplicate_empty_and_unknown_run_arguments) {
    const std::vector<std::vector<std::string>> cases = {
        {"agent"},
        {"agent", "unknown"},
        {"agent", "run", "--workspace", "."},
        {"agent", "run", "--issue", "test"},
        {"agent", "run", "--workspace", "", "--issue", "test"},
        {"agent", "run", "--workspace", ".", "--issue", ""},
        {"agent", "run", "--workspace", ".", "--workspace", "other",
         "--issue", "test"},
        {"agent", "run", "--workspace", ".", "--issue", "test",
         "--unexpected"},
    };
    for (const auto& args : cases) {
        std::size_t run_calls = 0;
        std::ostringstream out;
        std::ostringstream err;
        agent::CliApp app(
            [&](const agent::RunRequest&) {
                ++run_calls;
                return test::terminal_result(agent::TaskStatus::Completed);
            },
            [](const std::filesystem::path&) {
                return test::verified_state(agent::TaskStatus::Completed);
            },
            out, err);
        REQUIRE(app.execute(args) == agent::ExitCode::InvalidInputOrConfig);
        REQUIRE(run_calls == 0);
        REQUIRE(!err.str().empty());
    }
}

TEST_CASE(cli_dispatches_verify_log_and_prints_only_summary_fields) {
    std::vector<std::filesystem::path> paths;
    std::ostringstream out;
    std::ostringstream err;
    agent::CliApp app(
        [](const agent::RunRequest&) {
            return test::terminal_result(agent::TaskStatus::Completed);
        },
        [&](const std::filesystem::path& path) {
            paths.push_back(path);
            auto state = test::terminal_state(agent::TaskStatus::Failed);
            state.task_id = "task-0123456789abcdef0123456789abcdef";
            return agent::Result<agent::TaskState>::success(std::move(state));
        },
        out, err);

    REQUIRE(app.execute({"agent", "verify-log", "--events",
                         u8"E:/运行/事件.jsonl"}) == agent::ExitCode::Success);
    REQUIRE(paths == std::vector<std::filesystem::path>{
                         std::filesystem::u8path(u8"E:/运行/事件.jsonl")});
    REQUIRE(out.str().find("task-0123456789abcdef0123456789abcdef") !=
            std::string::npos);
    REQUIRE(out.str().find("Failed") != std::string::npos);
    REQUIRE(out.str().find("9") != std::string::npos);
    REQUIRE(out.str().find("SENTINEL_PRIVATE_FAILURE_DETAIL") ==
            std::string::npos);
    REQUIRE(err.str().empty());
}

TEST_CASE(cli_maps_verify_failure_to_invalid_event_log_without_leaking_details) {
    std::ostringstream out;
    std::ostringstream err;
    agent::CliApp app(
        [](const agent::RunRequest&) {
            return test::terminal_result(agent::TaskStatus::Completed);
        },
        [](const std::filesystem::path&) {
            return agent::Result<agent::TaskState>::failure(
                {agent::ErrorCode::PersistenceFailure,
                 "SENTINEL_INVALID_LOG_DETAIL", false});
        },
        out, err);
    REQUIRE(app.execute(
                {"agent", "verify-log", "--events", "events.jsonl"}) ==
            agent::ExitCode::InvalidEventLog);
    REQUIRE(out.str().find("SENTINEL_INVALID_LOG_DETAIL") == std::string::npos);
    REQUIRE(err.str().find("SENTINEL_INVALID_LOG_DETAIL") ==
            std::string::npos);
}

TEST_CASE(cli_rejects_verify_log_without_the_events_binding) {
    std::size_t verify_calls = 0;
    std::ostringstream out;
    std::ostringstream err;
    agent::CliApp app(
        [](const agent::RunRequest&) {
            return test::terminal_result(agent::TaskStatus::Completed);
        },
        [&](const std::filesystem::path&) {
            ++verify_calls;
            return test::verified_state(agent::TaskStatus::Completed);
        },
        out, err);

    REQUIRE(app.execute({"agent", "verify-log", "events.jsonl"}) ==
            agent::ExitCode::InvalidInputOrConfig);
    REQUIRE(verify_calls == 0);
}

TEST_CASE(cli_rejects_an_unsafe_replayed_task_id_without_printing_it) {
    const std::string unsafe_task_id =
        "task-0123456789abcdef\r\nINJECTED=" + std::string(4096, 'x');
    std::ostringstream out;
    std::ostringstream err;
    agent::CliApp app(
        [](const agent::RunRequest&) {
            return test::terminal_result(agent::TaskStatus::Completed);
        },
        [&](const std::filesystem::path&) {
            auto state = test::terminal_state(agent::TaskStatus::Failed);
            state.task_id = unsafe_task_id;
            return agent::Result<agent::TaskState>::success(std::move(state));
        },
        out, err);

    REQUIRE(app.execute(
                {"agent", "verify-log", "--events", "events.jsonl"}) ==
            agent::ExitCode::InvalidEventLog);
    REQUIRE(out.str().empty());
    REQUIRE(err.str().find("INJECTED") == std::string::npos);
    REQUIRE(err.str().size() < 128);
}

TEST_CASE(config_loads_defaults_and_api_key_authentication) {
    const auto config = agent::load_runtime_config(fixtures::valid_environment());
    REQUIRE(config.has_value());
    REQUIRE(config.value().anthropic.base_url == "https://provider.example");
    REQUIRE(config.value().anthropic.model == "model-id");
    REQUIRE(config.value().anthropic.credential_kind ==
            agent::CredentialKind::ApiKey);
    REQUIRE(config.value().anthropic.credential == "test-api-key");
    REQUIRE(config.value().anthropic.api_version == "2023-06-01");
    REQUIRE(config.value().anthropic.max_tokens == 4096);
    REQUIRE(config.value().budgets.max_model_rounds == 16);
    REQUIRE(config.value().budgets.max_tool_calls == 64);
    REQUIRE(config.value().budgets.max_task_time_ms == 1'800'000);
    REQUIRE(config.value().budgets.model_timeout_ms == 120'000);
    REQUIRE(config.value().runtime_root == std::filesystem::path("runtime_data"));
    REQUIRE(config.value().system_prompt ==
            "You are a coding agent runtime. Use only tools explicitly provided.");
}

TEST_CASE(config_loads_explicit_values_and_bearer_authentication) {
    test::MapEnvironment env{
        {"AGENT_BASE_URL", "https://provider.example/root/"},
        {"AGENT_MODEL", "model-id"},
        {"AGENT_AUTH_TOKEN", "test-bearer-token"},
        {"AGENT_MAX_TOKENS", "8192"},
        {"AGENT_MAX_MODEL_ROUNDS", "7"},
        {"AGENT_MAX_TOOL_CALLS", "11"},
        {"AGENT_MAX_TASK_SECONDS", "31"},
        {"AGENT_MODEL_TIMEOUT_SECONDS", "13"},
        {"AGENT_RUNTIME_ROOT", u8"运行数据"},
        {"AGENT_SYSTEM_PROMPT", u8"仅使用已提供的工具。"},
    };
    const auto config = agent::load_runtime_config(env);
    REQUIRE(config.has_value());
    REQUIRE(config.value().anthropic.credential_kind ==
            agent::CredentialKind::Bearer);
    REQUIRE(config.value().anthropic.credential == "test-bearer-token");
    REQUIRE(config.value().anthropic.max_tokens == 8192);
    REQUIRE((config.value().budgets ==
             agent::RuntimeBudgets{7, 11, 31'000, 13'000}));
    REQUIRE(config.value().runtime_root == std::filesystem::u8path(u8"运行数据"));
    REQUIRE(config.value().system_prompt == u8"仅使用已提供的工具。");
}

TEST_CASE(config_rejects_missing_or_multiple_authentication_modes) {
    const std::vector<test::MapEnvironment> cases = {
        {{"AGENT_BASE_URL", "https://provider.example"},
         {"AGENT_MODEL", "model-id"}},
        {{"AGENT_BASE_URL", "https://provider.example"},
         {"AGENT_MODEL", "model-id"},
         {"AGENT_API_KEY", "key"},
         {"AGENT_AUTH_TOKEN", "token"}},
        {{"AGENT_BASE_URL", "https://provider.example"},
         {"AGENT_MODEL", "model-id"},
         {"AGENT_API_KEY", ""}},
    };
    for (const auto& env : cases) {
        const auto config = agent::load_runtime_config(env);
        REQUIRE(!config.has_value());
        REQUIRE(config.error().code == agent::ErrorCode::InvalidConfiguration);
        REQUIRE(config.error().message.find("key") == std::string::npos);
        REQUIRE(config.error().message.find("token") == std::string::npos);
    }
}

TEST_CASE(config_rejects_missing_url_model_and_empty_runtime_root) {
    const std::vector<test::MapEnvironment> cases = {
        {{"AGENT_MODEL", "model-id"}, {"AGENT_API_KEY", "credential"}},
        {{"AGENT_BASE_URL", "https://provider.example"},
         {"AGENT_API_KEY", "credential"}},
        {{"AGENT_BASE_URL", "https://provider.example"},
         {"AGENT_MODEL", "model-id"},
         {"AGENT_API_KEY", "credential"},
         {"AGENT_RUNTIME_ROOT", ""}},
    };
    for (const auto& env : cases) {
        const auto config = agent::load_runtime_config(env);
        REQUIRE(!config.has_value());
        REQUIRE(config.error().code == agent::ErrorCode::InvalidConfiguration);
    }
}

TEST_CASE(config_rejects_zero_negative_malformed_and_overflowing_numbers) {
    const std::vector<std::pair<std::string, std::string>> invalid_values = {
        {"AGENT_MAX_TOKENS", "0"},
        {"AGENT_MAX_MODEL_ROUNDS", "-1"},
        {"AGENT_MAX_TOOL_CALLS", "12tools"},
        {"AGENT_MAX_TASK_SECONDS", "9223372036854776"},
        {"AGENT_MODEL_TIMEOUT_SECONDS", "18446744073709551616"},
    };
    for (const auto& invalid : invalid_values) {
        test::MapEnvironment env{
            {"AGENT_BASE_URL", "https://provider.example"},
            {"AGENT_MODEL", "model-id"},
            {"AGENT_API_KEY", "credential"},
            {invalid.first, invalid.second},
        };
        const auto config = agent::load_runtime_config(env);
        REQUIRE(!config.has_value());
        REQUIRE(config.error().code == agent::ErrorCode::InvalidConfiguration);
        REQUIRE(config.error().message.find(invalid.second) == std::string::npos);
    }
}

TEST_CASE(startup_parser_extracts_one_explicit_env_file_before_command_dispatch) {
    const auto parsed = agent::parse_startup_arguments(
        {"agent", "--env-file", u8"配置/agent.env", "run", "--workspace", ".",
         "--issue", "test"});
    REQUIRE(parsed.has_value());
    REQUIRE(parsed.value().env_file.has_value());
    REQUIRE(parsed.value().env_file->is_absolute());
    REQUIRE(parsed.value().env_file->filename() ==
            std::filesystem::u8path(u8"agent.env"));
    REQUIRE((parsed.value().command_args ==
             std::vector<std::string>{"agent", "run", "--workspace", ".",
                                      "--issue", "test"}));
}

TEST_CASE(startup_parser_rejects_env_file_with_verify_log_and_bad_forms) {
    const auto verify_with_env = agent::parse_startup_arguments(
        {"agent", "verify-log", "--events", "events.jsonl", "--env-file",
         "agent.env"});
    REQUIRE(!verify_with_env.has_value());
    REQUIRE(verify_with_env.error().code == agent::ErrorCode::InvalidInput);

    const std::vector<std::vector<std::string>> rejected = {
        {"agent", "--env-file"},
        {"agent", "--env-file", ""},
        {"agent", "--env-file", "one.env", "--env-file", "two.env", "run"},
    };
    for (const auto& args : rejected) {
        const auto parsed = agent::parse_startup_arguments(args);
        REQUIRE(!parsed.has_value());
        REQUIRE(parsed.error().code == agent::ErrorCode::InvalidInput);
    }
}

#if defined(_WIN32)
TEST_CASE(windows_wide_arguments_are_converted_to_utf8_at_the_entry_boundary) {
    const wchar_t* wide_args[] = {
        L"agent.exe", L"run", L"--workspace", L"E:/工作区/项目",
        L"--issue", L"修复警告\U0001F642"};
    const auto converted = agent::utf8_arguments_from_windows(
        static_cast<int>(std::size(wide_args)), wide_args);

    REQUIRE(converted.has_value());
    REQUIRE(converted.value().at(3) == u8"E:/工作区/项目");
    REQUIRE(converted.value().at(5) == u8"修复警告🙂");
}
#endif

TEST_CASE(explicit_env_loader_does_not_print_malformed_secret_lines) {
    test::ScopedTempDir temp("explicit-env-file");
    const auto file = temp.write_text(
        "agent.env",
        "TASK8_PRIVATE_VALUE=SENTINEL_ENV_SECRET_$TASK8_UNDEFINED_VALUE\n");
    std::ostringstream captured;
    auto* previous = std::cout.rdbuf(captured.rdbuf());
    const auto loaded = agent::load_explicit_env_file(file);
    std::cout.rdbuf(previous);

    REQUIRE(loaded.has_value());
    REQUIRE(captured.str().find("SENTINEL_ENV_SECRET") == std::string::npos);
}

TEST_CASE(explicit_env_loader_reads_a_file_under_a_unicode_path) {
    constexpr const char* kProbeName = "TASK8_DOTENV_PATH_PROBE";
#if defined(_WIN32)
    _putenv_s(kProbeName, "");
#else
    unsetenv(kProbeName);
#endif
    test::ScopedTempDir temp(std::filesystem::u8path(u8"显式环境文件"));
    const auto file = temp.write_text("agent.env",
                                      std::string(kProbeName) + "=loaded\n");
    const auto loaded = agent::load_explicit_env_file(file);
    agent::ProcessEnvironment environment;
    const auto observed = environment.get(kProbeName);
#if defined(_WIN32)
    _putenv_s(kProbeName, "");
#else
    unsetenv(kProbeName);
#endif

    REQUIRE(loaded.has_value());
    REQUIRE(observed == std::optional<std::string>{"loaded"});
}

#if defined(_WIN32)
TEST_CASE(process_environment_reads_unicode_runtime_root_and_prompt_as_utf8) {
    test::ScopedWideEnvironment base_url(
        L"AGENT_BASE_URL", L"https://provider.example");
    test::ScopedWideEnvironment model(L"AGENT_MODEL", L"model-id");
    test::ScopedWideEnvironment api_key(L"AGENT_API_KEY", L"test-key");
    test::ScopedWideEnvironment auth_token(L"AGENT_AUTH_TOKEN", L"");
    test::ScopedWideEnvironment runtime_root(
        L"AGENT_RUNTIME_ROOT", L"E:/运行根/🙂");
    test::ScopedWideEnvironment prompt(
        L"AGENT_SYSTEM_PROMPT", L"只保留中文提示🙂");
    agent::ProcessEnvironment environment;

    const auto observed_root = environment.get("AGENT_RUNTIME_ROOT");
    const auto observed_prompt = environment.get("AGENT_SYSTEM_PROMPT");
    const auto config = agent::load_runtime_config(environment);

    REQUIRE(observed_root == std::optional<std::string>{u8"E:/运行根/🙂"});
    REQUIRE(observed_prompt ==
            std::optional<std::string>{u8"只保留中文提示🙂"});
    REQUIRE(config.has_value());
    REQUIRE(config.value().runtime_root ==
            std::filesystem::u8path(u8"E:/运行根/🙂"));
    REQUIRE(config.value().system_prompt == u8"只保留中文提示🙂");
}
#endif

TEST_CASE(empty_adapters_expose_no_tools_and_no_evidence) {
    agent::EmptyToolGateway tools;
    agent::EmptyKnowledgeProvider knowledge;
    agent::TaskState state;
    const auto evidence = knowledge.retrieve(state);
    REQUIRE(tools.definitions().empty());
    REQUIRE(evidence.has_value());
    REQUIRE(evidence.value().items.empty());
    const auto execution = tools.execute(
        {"unexpected", "missing", agent::Value::object({})});
    REQUIRE(!execution.has_value());
    REQUIRE(execution.error().code == agent::ErrorCode::DependencyUnavailable);
}

TEST_CASE(system_clock_and_random_ids_follow_public_formats) {
    agent::SystemClock clock;
    const auto first_tick = clock.monotonic_ms();
    const auto timestamp = clock.now_utc();
    const auto second_tick = clock.monotonic_ms();
    REQUIRE(second_tick >= first_tick);
    REQUIRE(std::regex_match(
        timestamp,
        std::regex(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$)")));

    agent::RandomIdGenerator ids;
    const auto task_id = ids.next_task_id();
    const auto correlation_id = ids.next_correlation_id();
    REQUIRE(std::regex_match(task_id,
                             std::regex(R"(^task-[0-9a-f]{32}$)")));
    REQUIRE(std::regex_match(correlation_id,
                             std::regex(R"(^corr-[0-9a-f]{32}$)")));
    REQUIRE(task_id.substr(5) != correlation_id.substr(5));
}

TEST_CASE(signal_cancellation_observes_sigint_process_wide) {
    agent::SignalCancellation first;
    agent::SignalCancellation second;
    REQUIRE(!first.requested());
    REQUIRE(!second.requested());

    REQUIRE(std::raise(SIGINT) == 0);

    REQUIRE(first.requested());
    REQUIRE(second.requested());
}

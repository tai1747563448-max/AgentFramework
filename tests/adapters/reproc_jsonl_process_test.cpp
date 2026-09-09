#include "adapters/process/reproc_jsonl_process.h"
#include "ports/jsonl_process.h"
#include "test_support.h"

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>

namespace fixtures {

agent::JsonlProcessRequest request(const std::filesystem::path& cwd,
                                   std::vector<std::string> arguments) {
    std::vector<std::string> all{"-E", "-s", "-X", "utf8",
                                 AGENT_JSONL_FIXTURE_PATH};
    all.insert(all.end(), arguments.begin(), arguments.end());
    return {AGENT_JSONL_PYTHON_EXECUTABLE, std::move(all), cwd,
            2'000, 65'536, 4'096};
}

}  // namespace fixtures

TEST_CASE(reproc_jsonl_process_preserves_unicode_and_reuses_one_child) {
    test::ScopedTempDir temp(std::filesystem::u8path(u8"jsonl-进程"));
    agent::ReprocJsonlProcess process;

    const auto started = process.start(
        fixtures::request(temp.path(), {"echo", u8"参数🙂", "&|><\""}));

    REQUIRE(started.has_value());
    REQUIRE(started.value().find(u8"参数🙂") != std::string::npos);
    REQUIRE(process.running());
    const auto first = process.exchange(R"({"id":1})", 1'000);
    const auto second = process.exchange(R"({"id":2})", 1'000);
    if (!first.has_value()) {
        throw std::runtime_error(
            "first exchange error=" +
            std::to_string(static_cast<int>(first.error().code)) +
            " stderr=" + process.stderr_tail());
    }
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(first.value() == R"({"id":1})");
    REQUIRE(second.value() == R"({"id":2})");
    REQUIRE(process.running());
    REQUIRE(process.stop(1'000).has_value());
    REQUIRE(!process.running());
}

TEST_CASE(reproc_jsonl_process_drains_bounded_stderr_without_deadlock) {
    test::ScopedTempDir temp("jsonl-stderr");
    agent::ReprocJsonlProcess process;
    auto request = fixtures::request(temp.path(), {"stderr-spam", "200000"});
    request.max_stderr_bytes = 127;
    REQUIRE(process.start(request).has_value());

    const auto response = process.exchange(R"({"request":1})", 3'000);

    REQUIRE(response.has_value());
    REQUIRE(response.value() == R"({"request":1})");
    for (int attempt = 0; attempt < 100 && !process.stderr_truncated();
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(process.stderr_tail().size() <= 127);
    REQUIRE(process.stderr_truncated());
}

TEST_CASE(reproc_jsonl_process_forwards_only_strict_safe_progress) {
    test::ScopedTempDir temp("jsonl-progress");
    std::vector<std::string> messages;
    agent::ReprocJsonlProcess process(
        [&](const std::string& message) { messages.push_back(message); });
    REQUIRE(process.start(fixtures::request(temp.path(), {"progress"})).has_value());
    REQUIRE(process.exchange(R"({"request":1})", 1'000).has_value());
    REQUIRE(process.stop(1'000).has_value());

    REQUIRE(messages.size() == 1);
    REQUIRE(messages.front().find("RAG sidecar-model running: 1/2") == 0);
    REQUIRE(messages.front().find("throughput 0.333 items/s") !=
            std::string::npos);
    REQUIRE(messages.front().find("ETA 3s") != std::string::npos);
    REQUIRE(messages.front().find("SENTINEL") == std::string::npos);
}

TEST_CASE(reproc_jsonl_process_rejects_inconsistent_and_overlong_progress) {
    for (const auto* mode : {"invalid-progress", "oversized-progress-prefix"}) {
        test::ScopedTempDir temp(mode);
        std::vector<std::string> messages;
        agent::ReprocJsonlProcess process(
            [&](const std::string& message) { messages.push_back(message); });
        REQUIRE(process.start(fixtures::request(temp.path(), {mode})).has_value());
        REQUIRE(process.exchange(R"({"request":1})", 1'000).has_value());
        REQUIRE(process.stop(1'000).has_value());
        REQUIRE(messages.empty());
    }
}

TEST_CASE(reproc_jsonl_process_preserves_the_username_required_by_python_getpass) {
    test::ScopedTempDir temp("jsonl-getpass");
    agent::ReprocJsonlProcess process;

    const auto started =
        process.start(fixtures::request(temp.path(), {"getpass"}));

    REQUIRE(started.has_value());
    REQUIRE(process.stop(1'000).has_value());
}

TEST_CASE(reproc_jsonl_process_startup_timeout_stops_child) {
    test::ScopedTempDir temp("jsonl-start-timeout");
    agent::ReprocJsonlProcess process;
    auto request = fixtures::request(temp.path(), {"delayed-ready", "1500"});
    request.startup_timeout_ms = 100;

    const auto result = process.start(request);

    REQUIRE(!result.has_value());
    REQUIRE(result.error().code == agent::ErrorCode::RequestTimeout);
    REQUIRE(!process.running());
}

TEST_CASE(reproc_jsonl_process_query_timeout_invalidates_child) {
    test::ScopedTempDir temp("jsonl-query-timeout");
    agent::ReprocJsonlProcess process;
    REQUIRE(process.start(
                fixtures::request(temp.path(), {"query-delay", "1000"}))
                .has_value());

    const auto result = process.exchange(R"({"request":1})", 50);

    REQUIRE(!result.has_value());
    REQUIRE(result.error().code == agent::ErrorCode::RequestTimeout);
    REQUIRE(!process.running());
}

TEST_CASE(reproc_jsonl_process_rejects_oversized_line_and_unexpected_exit) {
    test::ScopedTempDir temp("jsonl-protocol-loss");
    {
        agent::ReprocJsonlProcess process;
        auto request = fixtures::request(temp.path(), {"oversized", "1000"});
        request.max_stdout_line_bytes = 128;
        REQUIRE(process.start(request).has_value());
        const auto result = process.exchange(R"({"request":1})", 1'000);
        REQUIRE(!result.has_value());
        REQUIRE(result.error().code == agent::ErrorCode::ProtocolFailure);
        REQUIRE(!process.running());
    }
    {
        agent::ReprocJsonlProcess process;
        REQUIRE(process.start(
                    fixtures::request(temp.path(), {"unexpected-exit"}))
                    .has_value());
        const auto result = process.exchange(R"({"request":2})", 1'000);
        REQUIRE(!result.has_value());
        REQUIRE(result.error().code == agent::ErrorCode::DependencyUnavailable);
        REQUIRE(!process.running());
    }
}

TEST_CASE(reproc_jsonl_process_rejects_nul_and_invalid_utf8_requests) {
    test::ScopedTempDir temp("jsonl-invalid-input");
    agent::ReprocJsonlProcess process;
    REQUIRE(process.start(fixtures::request(temp.path(), {"echo"})).has_value());

    const auto nul = process.exchange(std::string("a\0b", 3), 1'000);
    const auto invalid = process.exchange(std::string("\xFF", 1), 1'000);

    REQUIRE(!nul.has_value());
    REQUIRE(nul.error().code == agent::ErrorCode::InvalidInput);
    REQUIRE(!invalid.has_value());
    REQUIRE(invalid.error().code == agent::ErrorCode::InvalidInput);
    REQUIRE(process.running());
}

TEST_CASE(reproc_jsonl_process_destructor_terminates_descendant_tree) {
    test::ScopedTempDir temp("jsonl-tree");
    const auto ready = temp.path() / "child-ready.txt";
    const auto survived = temp.path() / "child-survived.txt";
    {
        agent::ReprocJsonlProcess process;
        REQUIRE(process.start(fixtures::request(
                    temp.path(),
                    {"tree-parent", ready.generic_u8string(),
                     survived.generic_u8string(), "1200"}))
                    .has_value());
        for (int attempt = 0; attempt < 100 && !std::filesystem::exists(ready);
             ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        REQUIRE(std::filesystem::exists(ready));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1'500));
    REQUIRE(!std::filesystem::exists(survived));
}

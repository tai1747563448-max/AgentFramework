#include "adapters/rag/python_rag_knowledge_provider.h"
#include "adapters/workspace/workspace_text.h"
#include "ports/process_runner.h"
#include "test_support.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <iostream>
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
    agent::ProcessOutput output;
    std::optional<agent::RuntimeError> failure;
};

struct Files {
    explicit Files(const std::string& name)
        : temporary(std::filesystem::path("agent-rag-adapter-" + name)),
          script(std::filesystem::weakly_canonical(
              temporary.write_text("rag/agent_rag_cli.py", "# fixture\n"))),
          index(std::filesystem::weakly_canonical(
              temporary.write_text("index/knowledge.sqlite3", "fixture"))) {}

    agent::PythonRagConfig config() const {
        return {"trusted-python", script, index, 5, 10};
    }

    test::ScopedTempDir temporary;
    std::filesystem::path script;
    std::filesystem::path index;
};

agent::TaskState state(std::string issue = u8"修复 parseIssue 中的 warning") {
    agent::TaskState value;
    value.issue = std::move(issue);
    return value;
}

nlohmann::json citation(std::string path,
                        std::int64_t start_line,
                        std::int64_t end_line,
                        std::string content,
                        std::optional<std::size_t> part = std::nullopt) {
    const auto digest = agent::workspace::sha256_hex(content);
    auto source_id = path + "#L" + std::to_string(start_line) + "-L" +
                     std::to_string(end_line);
    if (part.has_value()) {
        source_id += "-P" + std::to_string(*part);
    }
    return {{"source_id", std::move(source_id)},
            {"content", std::move(content)},
            {"metadata",
             {{"path", std::move(path)},
              {"start_line", start_line},
              {"end_line", end_line},
              {"sha256", digest},
              {"score", 3.125}}}};
}

nlohmann::json item() {
    return citation("guide.md", 10, 24, u8"证据 parseIssue");
}

std::string response(std::vector<nlohmann::json> items = {item()}) {
    return nlohmann::json{{"schema_version", 1}, {"items", std::move(items)}}
        .dump();
}

void require_fixed_error(const agent::Result<agent::EvidencePack>& result,
                         agent::ErrorCode code,
                         const std::string& message,
                         bool retryable) {
    REQUIRE(!result.has_value());
    REQUIRE(result.error().code == code);
    REQUIRE(result.error().message == message);
    REQUIRE(result.error().retryable == retryable);
    REQUIRE(result.error().message.find("SENTINEL_SECRET") ==
            std::string::npos);
}

}  // namespace fixtures

TEST_CASE(python_rag_adapter_sends_one_exact_bounded_process_request) {
    fixtures::Files files("request");
    fixtures::FakeProcessRunner process;
    process.output.stdout_utf8 = fixtures::response();
    agent::PythonRagKnowledgeProvider provider(process, files.config());
    const auto issue = u8"修复 \"parseIssue\"\n只检索这个问题";

    const auto result = provider.retrieve(fixtures::state(issue));

    REQUIRE(result.has_value());
    REQUIRE(process.requests.size() == 1);
    const auto& request = process.requests.front();
    REQUIRE(request.program == "trusted-python");
    REQUIRE((request.arguments ==
             std::vector<std::string>{"-E", "-s", "-X", "utf8",
                                      files.script.generic_u8string(), "query",
                                      "--index", files.index.generic_u8string()}));
    REQUIRE(request.working_directory == files.script.parent_path());
    REQUIRE(request.timeout_ms == 10'000);
    REQUIRE(request.max_stdout_bytes == 64 * 1024);
    REQUIRE(request.max_stderr_bytes == 4 * 1024);
    REQUIRE(request.environment_overrides.empty());
    const auto input = nlohmann::json::parse(request.stdin_utf8);
    REQUIRE(input == nlohmann::json({{"schema_version", 1},
                                     {"query", issue},
                                     {"top_k", 5},
                                     {"max_total_bytes", 32'768}}));
    for (const auto& argument : request.arguments) {
        REQUIRE(argument.find(issue) == std::string::npos);
    }
}

TEST_CASE(python_rag_adapter_preserves_item_order_and_exact_metadata) {
    fixtures::Files files("mapping");
    fixtures::FakeProcessRunner process;
    auto first = fixtures::citation("a.cpp", 1, 2, "first");
    first["metadata"]["score"] = 1.5;
    auto second = fixtures::citation(u8"文档.md", 3, 4, u8"第二条");
    second["metadata"]["score"] = 1;
    auto split = fixtures::citation(
        "generated.txt", 7, 7, "split line", std::size_t{2});
    split["metadata"]["score"] = 0.5;
    process.output.stdout_utf8 = fixtures::response({first, second, split});
    agent::PythonRagKnowledgeProvider provider(process, files.config());

    const auto result = provider.retrieve(fixtures::state());

    REQUIRE(result.has_value());
    REQUIRE(result.value().items.size() == 3);
    REQUIRE(result.value().items[0].source_id == "a.cpp#L1-L2");
    REQUIRE(result.value().items[0].content == "first");
    REQUIRE(result.value().items[0].metadata == agent::Value::object(
                {{"end_line", agent::Value(std::int64_t{2})},
                 {"path", agent::Value("a.cpp")},
                 {"score", agent::Value(1.5)},
                 {"sha256", agent::Value(agent::workspace::sha256_hex("first"))},
                 {"start_line", agent::Value(std::int64_t{1})}}));
    REQUIRE(result.value().items[1].source_id == u8"文档.md#L3-L4");
    REQUIRE(result.value().items[1].content == u8"第二条");
    REQUIRE(result.value().items[2].source_id ==
            "generated.txt#L7-L7-P2");
}

TEST_CASE(python_rag_adapter_rejects_forged_citation_metadata) {
    fixtures::Files files("forged-citation");
    const auto valid = fixtures::item();
    std::vector<nlohmann::json> invalid;

    auto missing_key = valid;
    missing_key["metadata"].erase("score");
    invalid.push_back(std::move(missing_key));
    auto extra_key = valid;
    extra_key["metadata"]["extra"] = true;
    invalid.push_back(std::move(extra_key));

    for (const auto& path : {"C:/private.md", "/private.md", "../private.md",
                             "docs/../private.md", "docs\\private.md"}) {
        auto item = valid;
        item["metadata"]["path"] = path;
        item["source_id"] = std::string(path) + "#L10-L24";
        invalid.push_back(std::move(item));
    }

    const std::vector<std::pair<nlohmann::json, std::string>> invalid_lines{
        {nlohmann::json(0), "guide.md#L0-L24"},
        {nlohmann::json(-1), "guide.md#L-1-L24"},
        {nlohmann::json(1.5), "guide.md#L1-L24"},
        {nlohmann::json(true), "guide.md#L1-L24"}};
    for (const auto& line : invalid_lines) {
        auto item = valid;
        item["metadata"]["start_line"] = line.first;
        item["source_id"] = line.second;
        invalid.push_back(std::move(item));
    }
    auto reversed_lines = valid;
    reversed_lines["metadata"]["start_line"] = 25;
    reversed_lines["metadata"]["end_line"] = 24;
    reversed_lines["source_id"] = "guide.md#L25-L24";
    invalid.push_back(std::move(reversed_lines));

    for (const auto& sha : std::vector<std::string>{
             "not-a-sha", std::string(64, 'A'), std::string(64, '0')}) {
        auto item = valid;
        item["metadata"]["sha256"] = sha;
        invalid.push_back(std::move(item));
    }
    for (const auto& score : {nlohmann::json(-1), nlohmann::json(true),
                              nlohmann::json("high")}) {
        auto item = valid;
        item["metadata"]["score"] = score;
        invalid.push_back(std::move(item));
    }

    for (const auto& source_id : {"forged-citation", "other.md#L10-L24",
                                  "guide.md#L9-L24", "guide.md#L10-L24-P0",
                                  "guide.md#L10-L24-P1"}) {
        auto item = valid;
        item["source_id"] = source_id;
        invalid.push_back(std::move(item));
    }

    for (const auto& item : invalid) {
        fixtures::FakeProcessRunner process;
        process.output.stdout_utf8 = fixtures::response({item});
        agent::PythonRagKnowledgeProvider provider(process, files.config());
        fixtures::require_fixed_error(
            provider.retrieve(fixtures::state()),
            agent::ErrorCode::ProtocolFailure, "invalid rag response", false);
    }
}

TEST_CASE(python_rag_adapter_maps_process_failures_without_leaking_raw_text) {
    fixtures::Files files("process-errors");
    fixtures::FakeProcessRunner process;
    agent::PythonRagKnowledgeProvider provider(process, files.config());

    process.failure = agent::RuntimeError{
        agent::ErrorCode::DependencyUnavailable,
        "SENTINEL_SECRET runner failure", true};
    fixtures::require_fixed_error(
        provider.retrieve(fixtures::state()),
        agent::ErrorCode::DependencyUnavailable, "rag process unavailable", true);

    process.failure.reset();
    process.output = {99, true, 10, "", "SENTINEL_SECRET stderr", false,
                      false};
    fixtures::require_fixed_error(
        provider.retrieve(fixtures::state()), agent::ErrorCode::RequestTimeout,
        "rag query timed out", true);

    process.output = {2, false, 10, "", "SENTINEL_SECRET stderr", false,
                      false};
    fixtures::require_fixed_error(
        provider.retrieve(fixtures::state()),
        agent::ErrorCode::DependencyUnavailable, "rag process failed", false);

    process.output = {0, false, 10, fixtures::response(),
                      "SENTINEL_SECRET stderr", true, false};
    fixtures::require_fixed_error(
        provider.retrieve(fixtures::state()), agent::ErrorCode::ProtocolFailure,
        "rag response exceeded limit", false);

    process.output = {0, false, 10,
                      fixtures::response({}) + std::string(65'536, ' '),
                      "SENTINEL_SECRET stderr", false, false};
    fixtures::require_fixed_error(
        provider.retrieve(fixtures::state()), agent::ErrorCode::ProtocolFailure,
        "rag response exceeded limit", false);
}

TEST_CASE(python_rag_adapter_rejects_invalid_config_and_query_before_launch) {
    fixtures::Files files("invalid-input");
    const auto valid = files.config();

    const std::vector<agent::PythonRagConfig> bad_configs{
        {"", valid.script_path, valid.index_path, 5, 10},
        {std::string(1, static_cast<char>(0xFF)), valid.script_path,
         valid.index_path, 5, 10},
        {"python", std::filesystem::path("relative.py"), valid.index_path, 5,
         10},
        {"python", valid.script_path, std::filesystem::path("relative.db"), 5,
         10},
        {"python", valid.script_path, valid.index_path, 0, 10},
        {"python", valid.script_path, valid.index_path, 21, 10},
        {"python", valid.script_path, valid.index_path, 5, 0},
        {"python", valid.script_path, valid.index_path, 5, 61}};
    for (const auto& config : bad_configs) {
        fixtures::FakeProcessRunner process;
        agent::PythonRagKnowledgeProvider provider(process, config);
        fixtures::require_fixed_error(
            provider.retrieve(fixtures::state()),
            agent::ErrorCode::InvalidConfiguration,
            "invalid rag configuration", false);
        REQUIRE(process.requests.empty());
    }

    for (const auto& issue :
         std::vector<std::string>{"", std::string(16'385, 'q'),
                                  std::string("a\0b", 3),
                                  std::string(1, static_cast<char>(0xFF))}) {
        fixtures::FakeProcessRunner process;
        agent::PythonRagKnowledgeProvider provider(process, valid);
        fixtures::require_fixed_error(
            provider.retrieve(fixtures::state(issue)),
            agent::ErrorCode::InvalidInput, "invalid rag query", false);
        REQUIRE(process.requests.empty());
    }
}

TEST_CASE(python_rag_adapter_rejects_every_owned_schema_violation) {
    fixtures::Files files("schema");
    const std::vector<std::string> bad_responses{
        "not-json",
        "[]",
        nlohmann::json{{"items", nlohmann::json::array()}}.dump(),
        nlohmann::json{{"schema_version", 1}}.dump(),
        nlohmann::json{{"schema_version", 1}, {"items", nlohmann::json::array()},
                       {"extra", true}}.dump(),
        R"({"schema_version":1,"schema_version":1,"items":[]})",
        nlohmann::json{{"schema_version", 2}, {"items", nlohmann::json::array()}}.dump(),
        nlohmann::json{{"schema_version", true}, {"items", nlohmann::json::array()}}.dump(),
        nlohmann::json{{"schema_version", 1}, {"items", "wrong"}}.dump(),
        nlohmann::json{{"schema_version", 1},
                       {"items", nlohmann::json::array({
                           {{"source_id", "source"}, {"content", "text"}}})}}.dump(),
        nlohmann::json{{"schema_version", 1},
                       {"items", nlohmann::json::array({
                           {{"source_id", "source"}, {"content", "text"},
                            {"metadata", nlohmann::json::object()},
                            {"extra", true}}})}}.dump(),
        nlohmann::json{{"schema_version", 1},
                       {"items", nlohmann::json::array({
                           {{"source_id", 1}, {"content", "text"},
                            {"metadata", nlohmann::json::object()}}})}}.dump(),
        nlohmann::json{{"schema_version", 1},
                       {"items", nlohmann::json::array({
                           {{"source_id", "source"}, {"content", 1},
                            {"metadata", nlohmann::json::object()}}})}}.dump(),
        nlohmann::json{{"schema_version", 1},
                       {"items", nlohmann::json::array({
                           {{"source_id", "source"}, {"content", "text"},
                            {"metadata", "wrong"}}})}}.dump()};

    for (const auto& output : bad_responses) {
        fixtures::FakeProcessRunner process;
        process.output.stdout_utf8 = output;
        process.output.stderr_utf8 = "SENTINEL_SECRET";
        agent::PythonRagKnowledgeProvider provider(process, files.config());
        fixtures::require_fixed_error(
            provider.retrieve(fixtures::state()),
            agent::ErrorCode::ProtocolFailure, "invalid rag response", false);
    }
}

TEST_CASE(python_rag_adapter_rejects_top_k_duplicates_utf8_and_bounds) {
    fixtures::Files files("response-bounds");
    auto config = files.config();
    config.top_k = 1;

    {
        fixtures::FakeProcessRunner process;
        process.output.stdout_utf8 = fixtures::response(
            {fixtures::citation("a.md", 1, 1, "a"),
             fixtures::citation("b.md", 1, 1, "b")});
        agent::PythonRagKnowledgeProvider provider(process, config);
        fixtures::require_fixed_error(
            provider.retrieve(fixtures::state()),
            agent::ErrorCode::ProtocolFailure, "invalid rag response", false);
    }

    config.top_k = 5;
    const auto long_path = std::string(507, 's');
    const auto long_source =
        fixtures::citation(long_path, 1, 1, "content");
    REQUIRE(long_source.at("source_id").get<std::string>().size() == 513);
    std::vector<std::string> bad_responses{
        fixtures::response({fixtures::item(), fixtures::item()}),
        fixtures::response({fixtures::citation("empty.md", 1, 1, "")}),
        fixtures::response({fixtures::citation(
            "oversized.md", 1, 1, std::string(8'193, 'x'))}),
        fixtures::response({long_source}),
        fixtures::response(
            {fixtures::citation("utf8.md", 1, 1, "content")})};
    const std::string content_prefix = "\"content\":\"";
    bad_responses.back().insert(
        bad_responses.back().find(content_prefix) + content_prefix.size(),
        std::string(1, static_cast<char>(0xFF)));

    for (std::size_t index = 0; index < bad_responses.size(); ++index) {
        fixtures::FakeProcessRunner process;
        process.output.stdout_utf8 = bad_responses.at(index);
        agent::PythonRagKnowledgeProvider provider(process, config);
        const auto result = provider.retrieve(fixtures::state());
        if (result.has_value()) {
            throw std::runtime_error(
                "response-bound case unexpectedly succeeded: " +
                std::to_string(index));
        }
        fixtures::require_fixed_error(
            result,
            agent::ErrorCode::ProtocolFailure, "invalid rag response", false);
    }

    config.top_k = 20;
    std::vector<nlohmann::json> total_too_large;
    for (std::size_t index = 0; index < 5; ++index) {
        total_too_large.push_back(fixtures::citation(
            "source-" + std::to_string(index) + ".md", 1, 1,
            std::string(7'000, 'x')));
    }
    fixtures::FakeProcessRunner process;
    process.output.stdout_utf8 = fixtures::response(total_too_large);
    agent::PythonRagKnowledgeProvider provider(process, config);
    fixtures::require_fixed_error(
        provider.retrieve(fixtures::state()),
        agent::ErrorCode::ProtocolFailure, "invalid rag response", false);
}

TEST_CASE(python_rag_adapter_rejects_linked_or_nonregular_inputs) {
    fixtures::Files files("links");
    auto config = files.config();
    fixtures::FakeProcessRunner process;

    config.index_path = std::filesystem::weakly_canonical(
        files.index.parent_path());
    agent::PythonRagKnowledgeProvider directory_provider(process, config);
    fixtures::require_fixed_error(
        directory_provider.retrieve(fixtures::state()),
        agent::ErrorCode::InvalidConfiguration,
        "invalid rag configuration", false);
    REQUIRE(process.requests.empty());
    config = files.config();

    const auto linked_index = files.temporary.path() / "linked-index.sqlite3";
    std::error_code error;
    std::filesystem::create_hard_link(files.index, linked_index, error);
    if (!error) {
        config.index_path = std::filesystem::weakly_canonical(linked_index);
        agent::PythonRagKnowledgeProvider provider(process, config);
        fixtures::require_fixed_error(
            provider.retrieve(fixtures::state()),
            agent::ErrorCode::InvalidConfiguration,
            "invalid rag configuration", false);
        REQUIRE(process.requests.empty());
    } else {
        std::cout << "SKIP hard-link RAG path regression: " << error.message()
                  << '\n';
    }
    error.clear();
    std::filesystem::remove(linked_index, error);

    error.clear();
    const auto linked_script = files.temporary.path() / "linked-script.py";
    std::filesystem::create_symlink(files.script, linked_script, error);
    if (!error) {
        config = files.config();
        config.script_path = linked_script;
        agent::PythonRagKnowledgeProvider provider(process, config);
        fixtures::require_fixed_error(
            provider.retrieve(fixtures::state()),
            agent::ErrorCode::InvalidConfiguration,
            "invalid rag configuration", false);
        REQUIRE(process.requests.empty());
    } else {
        std::cout << "SKIP symlink RAG path regression: " << error.message()
                  << '\n';
    }
}

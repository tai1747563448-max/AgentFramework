#include "adapters/rag/persistent_rag_knowledge_provider.h"
#include "adapters/rag/rag_protocol.h"
#include "adapters/workspace/workspace_text.h"
#include "ports/jsonl_process.h"
#include "ports/rag_pack_verifier.h"
#include "test_support.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fixtures {

std::string ready_line() {
    return nlohmann::json(
        {{"schema_version", 2},
         {"request_id", "req-00000000000000000000000000000000"},
         {"op", "ready"},
         {"payload",
          {{"pack_id", "pack-cccccccccccccccccccccccccccccccc"},
           {"retrieval_revision", "retrieval-" + std::string(64, 'e')},
           {"snapshot_date", "2026-09-03"},
           {"document_count", 30'000},
           {"chunk_count", 45'000},
           {"model", "BAAI/bge-m3"},
           {"revision", "dddddddddddddddddddddddddddddddddddddddd"},
           {"dimensions", 1024},
           {"device", "cpu"}}}})
        .dump();
}

std::string query_line(const std::string& request_line) {
    const auto request = nlohmann::json::parse(request_line);
    const std::string content = "Legal evidence";
    return nlohmann::json(
        {{"schema_version", 2},
         {"request_id", request.at("request_id")},
         {"op", "query_result"},
         {"payload",
          {{"outcome", "matched"},
           {"items",
            nlohmann::json::array(
                {{{"source_id", "doc-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-chunk-0000000000000001"},
                  {"content", content},
                  {"metadata",
                   {{"citation", "40 CFR 60.1"},
                    {"path", "corpus/title-040/section-60.1.md"},
                    {"start_line", 10},
                    {"end_line", 12},
                    {"snapshot_date", "2026-09-03"},
                    {"official_url", "https://www.ecfr.gov/on/2026-09-03/title-40/section-60.1"},
                    {"content_sha256", agent::workspace::sha256_hex(content)},
                    {"document_sha256", std::string(64, 'b')},
                    {"retrieval_revision", "retrieval-" + std::string(64, 'e')},
                    {"bm25_rank", 1},
                    {"dense_rank", 2},
                    {"fusion_score", 0.03},
                    {"is_neighbor", false},
                    {"neighbor_of", nullptr}}}}})}}}})
        .dump();
}

class FakeJsonlProcess final : public agent::JsonlProcess {
public:
    agent::Result<std::string> start(
        const agent::JsonlProcessRequest& request) override {
        ++start_count;
        starts.push_back(request);
        if (start_error.has_value()) {
            running_ = false;
            return agent::Result<std::string>::failure(*start_error);
        }
        running_ = true;
        return agent::Result<std::string>::success(ready);
    }

    agent::Result<std::string> exchange(const std::string& line,
                                        std::int64_t timeout_ms) override {
        ++exchange_count;
        exchanges.push_back(line);
        timeouts.push_back(timeout_ms);
        if (exchange_error.has_value()) {
            running_ = false;
            return agent::Result<std::string>::failure(*exchange_error);
        }
        return responder(line);
    }

    bool running() const noexcept override { return running_; }

    agent::Result<void> stop(std::int64_t) override {
        ++stop_count;
        running_ = false;
        return agent::Result<void>::success();
    }

    void crash() noexcept { running_ = false; }

    std::string ready{ready_line()};
    std::function<agent::Result<std::string>(const std::string&)> responder =
        [](const std::string& line) {
            return agent::Result<std::string>::success(query_line(line));
        };
    std::optional<agent::RuntimeError> start_error;
    std::optional<agent::RuntimeError> exchange_error;
    bool running_{false};
    int start_count{0};
    int exchange_count{0};
    int stop_count{0};
    std::vector<agent::JsonlProcessRequest> starts;
    std::vector<std::string> exchanges;
    std::vector<std::int64_t> timeouts;
};

class FakeRagPackVerifier final : public agent::RagPackVerifier {
public:
    agent::Result<void> verify_executable_payload(
        const std::filesystem::path& root) override {
        ++verify_count;
        roots.push_back(root);
        if (error.has_value()) {
            return agent::Result<void>::failure(*error);
        }
        return agent::Result<void>::success();
    }

    int verify_count{0};
    std::vector<std::filesystem::path> roots;
    std::optional<agent::RuntimeError> error;
};

agent::RagConfig config() {
    agent::RagConfig result;
    result.enabled = true;
    result.mode = "lexical";
    result.pack_root = "C:/AgentFramework-Knowledge/ecfr-2026-09-03";
    result.top_k = 6;
    result.max_total_bytes = 32'768;
    result.startup_timeout_ms = 120'000;
    result.query_timeout_ms = 30'000;
    result.device = "cpu";
    return result;
}

agent::TaskState state(std::string issue = "What does 40 CFR 60.1 say?") {
    agent::TaskState value;
    value.issue = std::move(issue);
    return value;
}

}  // namespace fixtures

TEST_CASE(persistent_rag_provider_starts_lazily_and_reuses_one_process) {
    fixtures::FakeJsonlProcess process;
    fixtures::FakeRagPackVerifier verifier;
    agent::PersistentRagKnowledgeProvider provider(
        process, verifier, fixtures::config());
    REQUIRE(process.start_count == 0);

    const auto first = provider.retrieve(fixtures::state());
    const auto second = provider.retrieve(fixtures::state("second question"));

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(process.start_count == 1);
    REQUIRE(verifier.verify_count == 1);
    REQUIRE(process.exchange_count == 2);
    REQUIRE(process.starts.front().startup_timeout_ms == 120'000);
    REQUIRE(process.starts.front().arguments.front() == "-B");
    REQUIRE(process.starts.front().arguments.size() >= 2);
    REQUIRE(process.starts.front().arguments[process.starts.front().arguments.size() - 2] ==
            "--device");
    REQUIRE(process.starts.front().arguments.back() == "cpu");
    REQUIRE(process.timeouts == std::vector<std::int64_t>({30'000, 30'000}));
    const auto first_request = nlohmann::json::parse(process.exchanges[0]);
    const auto second_request = nlohmann::json::parse(process.exchanges[1]);
    REQUIRE(first_request.at("op") == "query");
    REQUIRE(first_request.at("payload").at("mode") == "lexical");
    REQUIRE(first_request.at("payload").at("top_k") == 6);
    REQUIRE(first_request.at("payload").at("max_total_bytes") == 32'768);
    REQUIRE(first_request.at("request_id") != second_request.at("request_id"));
}

TEST_CASE(persistent_rag_provider_skips_commands_and_explicit_opt_out) {
    for (const auto& issue : {"/help", "  /new", u8"请回答，但不要查询知识库",
                              u8"不要用 RAG", "Do not use RAG for this answer",
                              "don't use the knowledge base"}) {
        fixtures::FakeJsonlProcess process;
        fixtures::FakeRagPackVerifier verifier;
        agent::PersistentRagKnowledgeProvider provider(
            process, verifier, fixtures::config());
        const auto result = provider.retrieve(fixtures::state(issue));
        REQUIRE(result.has_value());
        REQUIRE(result.value().items.empty());
        REQUIRE(process.start_count == 0);
        REQUIRE(process.exchange_count == 0);
        REQUIRE(verifier.verify_count == 0);
    }
}

TEST_CASE(persistent_rag_provider_restarts_only_after_prequery_crash) {
    fixtures::FakeJsonlProcess process;
    fixtures::FakeRagPackVerifier verifier;
    agent::PersistentRagKnowledgeProvider provider(
        process, verifier, fixtures::config());
    REQUIRE(provider.retrieve(fixtures::state()).has_value());
    process.crash();
    REQUIRE(provider.retrieve(fixtures::state("after crash")).has_value());
    REQUIRE(process.start_count == 2);
    REQUIRE(verifier.verify_count == 2);
    REQUIRE(process.exchange_count == 2);
}

TEST_CASE(persistent_rag_provider_does_not_replay_inflight_failure) {
    fixtures::FakeJsonlProcess process;
    fixtures::FakeRagPackVerifier verifier;
    process.exchange_error = agent::RuntimeError{
        agent::ErrorCode::RequestTimeout, "SENTINEL raw error", true};
    agent::PersistentRagKnowledgeProvider provider(
        process, verifier, fixtures::config());

    const auto result = provider.retrieve(fixtures::state());

    REQUIRE(!result.has_value());
    REQUIRE(result.error().code == agent::ErrorCode::RequestTimeout);
    REQUIRE(result.error().message == "rag query timed out");
    REQUIRE(result.error().retryable);
    REQUIRE(process.start_count == 1);
    REQUIRE(process.exchange_count == 1);
}

TEST_CASE(persistent_rag_provider_maps_malformed_handshake_and_stops_on_destroy) {
    fixtures::FakeJsonlProcess process;
    fixtures::FakeRagPackVerifier verifier;
    process.ready = R"({"schema_version":2,"request_id":"wrong"})";
    {
        agent::PersistentRagKnowledgeProvider provider(
            process, verifier, fixtures::config());
        const auto result = provider.retrieve(fixtures::state());
        REQUIRE(!result.has_value());
        REQUIRE(result.error().code == agent::ErrorCode::ProtocolFailure);
        REQUIRE(result.error().message == "invalid rag protocol response");
    }
    REQUIRE(process.stop_count >= 1);
}

TEST_CASE(persistent_rag_provider_never_starts_unverified_payload) {
    fixtures::FakeJsonlProcess process;
    fixtures::FakeRagPackVerifier verifier;
    verifier.error = agent::RuntimeError{
        agent::ErrorCode::DependencyUnavailable,
        "rag executable payload integrity check failed", false};
    agent::PersistentRagKnowledgeProvider provider(
        process, verifier, fixtures::config());

    const auto result = provider.retrieve(fixtures::state());

    REQUIRE(!result.has_value());
    REQUIRE(result.error().message ==
            "rag executable payload integrity check failed");
    REQUIRE(verifier.verify_count == 1);
    REQUIRE(process.start_count == 0);
}

TEST_CASE(persistent_rag_provider_rejects_invalid_inputs_before_starting) {
    for (const auto& issue : {std::string{}, std::string(16'385, 'x'),
                              std::string("a\0b", 3), std::string("\xff")}) {
        fixtures::FakeJsonlProcess process;
        fixtures::FakeRagPackVerifier verifier;
        agent::PersistentRagKnowledgeProvider provider(
            process, verifier, fixtures::config());
        const auto result = provider.retrieve(fixtures::state(issue));
        REQUIRE(!result.has_value());
        REQUIRE(result.error().code == agent::ErrorCode::InvalidInput);
        REQUIRE(process.start_count == 0);
        REQUIRE(verifier.verify_count == 0);
    }
    for (int mutation = 0; mutation != 7; ++mutation) {
        auto config = fixtures::config();
        switch (mutation) {
        case 0: config.pack_root = "relative"; break;
        case 1: config.mode = "unknown"; break;
        case 2: config.top_k = 0; break;
        case 3: config.max_total_bytes = 32'769; break;
        case 4: config.startup_timeout_ms = 0; break;
        case 5: config.query_timeout_ms = 600'001; break;
        case 6: config.device = "unknown"; break;
        }
        fixtures::FakeJsonlProcess process;
        fixtures::FakeRagPackVerifier verifier;
        agent::PersistentRagKnowledgeProvider provider(process, verifier, config);
        const auto result = provider.retrieve(fixtures::state());
        REQUIRE(!result.has_value());
        REQUIRE(result.error().code == agent::ErrorCode::InvalidConfiguration);
        REQUIRE(process.start_count == 0);
        REQUIRE(verifier.verify_count == 0);
    }
}

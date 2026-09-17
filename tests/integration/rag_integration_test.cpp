#include "adapters/persistence/jsonl_event_store.h"
#include "adapters/process/reproc_jsonl_process.h"
#include "adapters/rag/persistent_rag_knowledge_provider.h"
#include "adapters/workspace/workspace_tool_gateway.h"
#include "application/runtime_engine.h"
#include "application/state_reducer.h"
#include "ports/cancellation.h"
#include "ports/clock.h"
#include "ports/operation_context.h"
#include "ports/id_generator.h"
#include "ports/model_client.h"
#include "test_support.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifndef AGENT_RAG_PYTHON_EXECUTABLE
#error "AGENT_RAG_PYTHON_EXECUTABLE is required"
#endif

#ifndef AGENT_RAG_FIXTURE_PATH
#error "AGENT_RAG_FIXTURE_PATH is required"
#endif

namespace fixtures {

class FakeModel final : public agent::ModelClient {
public:
    explicit FakeModel(agent::ModelResponse response)
        : response_(std::move(response)) {}

    agent::Result<agent::ModelResponse> complete(
        const agent::ModelRequest& request) override {
        requests.push_back(request);
        return agent::Result<agent::ModelResponse>::success(response_);
    }

    std::vector<agent::ModelRequest> requests;

private:
    agent::ModelResponse response_;
};

class FakeClock final : public agent::Clock {
public:
    std::string now_utc() const override {
        return "2026-08-21T00:00:00.000Z";
    }

    std::int64_t monotonic_ms() const override {
        return 1'000;
    }
};

class FakeIds final : public agent::IdGenerator {
public:
    explicit FakeIds(std::string task_id) : task_id_(std::move(task_id)) {}

    std::string next_task_id() override {
        return task_id_;
    }

    std::string next_correlation_id() override {
        return "corr-rag-" + std::to_string(next_correlation_++);
    }

private:
    std::string task_id_;
    std::size_t next_correlation_{1};
};

class FakeCancellation final : public agent::Cancellation {
public:
    bool requested() const noexcept override {
        return false;
    }
};

// Substitute only release-pack/model loading. Pipes, v2 sidecar, hybrid
// index/retriever, C++ validation, runtime and persistence remain real.
class FixtureProcess final : public agent::JsonlProcess {
public:
    agent::Result<std::string> start(const agent::JsonlProcessRequest& request) override {
        ++starts;
        auto host = request;
        REQUIRE(host.arguments.size() == 11);
        REQUIRE(host.arguments[6] == "serve");
        host.program = AGENT_RAG_PYTHON_EXECUTABLE;
        host.arguments[5] = AGENT_RAG_FIXTURE_PATH;
        host.working_directory = std::filesystem::path(AGENT_RAG_FIXTURE_PATH).parent_path();
        return process.start(host);
    }
    agent::Result<std::string> exchange(const std::string& line, std::int64_t timeout) override {
        return process.exchange(line, timeout);
    }
    bool running() const noexcept override { return process.running(); }
    agent::Result<void> stop(std::int64_t timeout) override { return process.stop(timeout); }
    int starts{0};
private:
    agent::ReprocJsonlProcess process;
};

class FixtureVerifier final : public agent::RagPackVerifier {
public:
    agent::Result<void> verify_executable_payload(
        const std::filesystem::path&,
        const agent::OperationContext& /*context*/) override {
        return agent::Result<void>::success();
    }
};

agent::RagConfig rag_config(const std::filesystem::path& root) {
    agent::RagConfig config;
    config.enabled = true;
    config.pack_root = root;
    config.mode = "lexical";
    config.device = "cpu";
    config.top_k = 3;
    config.startup_timeout_ms = 30'000;
    config.query_timeout_ms = 10'000;
    return config;
}

agent::ModelResponse final_response() {
    return {{agent::TextBlock{u8"已根据本地知识回答"}},
            agent::StopReason::EndTurn, "end_turn", 20, 8,
            "provider-request-rag-integration"};
}

agent::RunRequest run_request(const std::string& issue,
                              const std::filesystem::path& workspace) {
    return {issue, workspace.generic_u8string(), u8"你是编码代理。",
            {4, 4, 30'000, 5'000}};
}

}  // namespace fixtures

TEST_CASE(persistent_rag_round_trip_persists_replays_and_blocks_bad_index) {
    test::ScopedTempDir temporary(std::filesystem::u8path(u8"rag-新版集成"));
    const auto pack_root = temporary.path() / "pack";
    const auto workspace = temporary.path() / std::filesystem::u8path(u8"工作区");
    const auto runtime_root = temporary.path() / "runtime";
    std::filesystem::create_directories(pack_root);
    std::filesystem::create_directories(workspace);
    std::filesystem::create_directories(runtime_root);
    fixtures::FixtureProcess process;
    fixtures::FixtureVerifier verifier;
    const auto config = fixtures::rag_config(pack_root);
    agent::PersistentRagKnowledgeProvider knowledge(process, verifier, config);
    agent::TaskState query_state;
    query_state.issue = u8"parseIssue 中文 warning";
    const auto first = knowledge.retrieve(query_state);
    REQUIRE(first.has_value());
    REQUIRE(first.value().items.size() == 1);
    const auto second = knowledge.retrieve(query_state);
    REQUIRE(second.has_value());
    REQUIRE(second.value() == first.value());
    REQUIRE(process.starts == 1);
    const auto& item = second.value().items.front();
    REQUIRE(item.content.find("parseIssue") != std::string::npos);
    REQUIRE(item.content.find(u8"中文") != std::string::npos);
    REQUIRE(item.metadata.at("citation").as_string() == "40 CFR 60.1");
    REQUIRE(item.metadata.at("path").as_string() == "corpus/title-040/section-60.1.md");
    REQUIRE(item.source_id.find(pack_root.generic_u8string()) == std::string::npos);
    REQUIRE(second.value().tool_use_forbidden);

    agent::JsonlEventStore store(runtime_root);
    agent::WorkspaceToolGateway tools(runtime_root);
    fixtures::FakeModel model(fixtures::final_response());
    fixtures::FakeClock clock;
    fixtures::FakeIds ids("task-0000000000000000000000000000000a");
    fixtures::FakeCancellation cancellation;
    agent::RuntimeEngine engine(model, tools, knowledge, store, clock, ids, cancellation);
    const auto result = engine.run(fixtures::run_request(query_state.issue, workspace), {});
    REQUIRE(result.state.has_value());
    REQUIRE(!result.fatal_error.has_value());
    REQUIRE(result.state->status == agent::TaskStatus::Completed);
    REQUIRE(result.state->evidence == second.value());
    REQUIRE(model.requests.size() == 1);
    REQUIRE(model.requests.front().evidence == second.value());
    REQUIRE(model.requests.front().tools.empty());
    REQUIRE(process.starts == 1);
    const auto event_file = store.event_path(result.state->task_id);
    REQUIRE(event_file.has_value());
    const auto events = store.read_file(event_file.value());
    REQUIRE(events.has_value());
    const auto replayed = agent::replay_events(events.value());
    REQUIRE(replayed.has_value());
    REQUIRE(replayed.value() == result.state.value());
    REQUIRE(replayed.value().evidence == second.value());

    // A valid but absent explicit authority must bypass the answer model.
    fixtures::FakeModel absent_model(fixtures::final_response());
    fixtures::FakeIds absent_ids("task-0000000000000000000000000000000c");
    agent::RuntimeEngine absent_engine(absent_model, tools, knowledge, store,
                                      clock, absent_ids, cancellation);
    const auto absent = absent_engine.run(
        fixtures::run_request("99 CFR 999999.999", workspace), {});
    REQUIRE(absent.state.has_value());
    REQUIRE(absent.state->status == agent::TaskStatus::Completed);
    REQUIRE(absent.state->evidence.authoritative_no_match);
    REQUIRE(absent_model.requests.empty());

    // Stop before corrupting the index so Windows releases all mapped files.
    REQUIRE(process.stop(1'000).has_value());
    {
        std::ofstream corrupted(pack_root / "index" / "metadata.sqlite3",
                                std::ios::binary | std::ios::trunc);
        corrupted << "not a sqlite index";
        REQUIRE(static_cast<bool>(corrupted));
    }
    fixtures::FakeModel unused_model(fixtures::final_response());
    fixtures::FakeIds broken_ids("task-0000000000000000000000000000000b");
    agent::RuntimeEngine broken_engine(unused_model, tools, knowledge, store,
                                      clock, broken_ids, cancellation);
    const auto broken = broken_engine.run(fixtures::run_request(query_state.issue, workspace), {});
    REQUIRE(broken.state.has_value());
    REQUIRE(!broken.fatal_error.has_value());
    REQUIRE(broken.state->status == agent::TaskStatus::Failed);
    REQUIRE(broken.state->terminal_error.has_value());
    REQUIRE(broken.state->terminal_error->code == agent::ErrorCode::DependencyUnavailable);
    REQUIRE(unused_model.requests.empty());
    const auto broken_file = store.event_path(broken.state->task_id);
    REQUIRE(broken_file.has_value());
    const auto broken_events = store.read_file(broken_file.value());
    REQUIRE(broken_events.has_value());
    REQUIRE(broken_events.value().size() == 3);
    REQUIRE(agent::event_kind(broken_events.value().back().payload) ==
            agent::EventKind::ContextPreparationFailed);
}

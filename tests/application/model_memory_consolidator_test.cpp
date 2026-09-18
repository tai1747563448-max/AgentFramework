#include "application/model_memory_consolidator.h"
#include "application/memory_policy.h"
#include "ports/model_client.h"
#include "test_support.h"

#include <nlohmann/json.hpp>
#include <functional>

namespace {
class RecordingModel final : public agent::ModelClient {
public:
    agent::ModelResponse response{{agent::TextBlock{R"({"memories":[]})"}},
                                  agent::StopReason::EndTurn};
    std::vector<agent::ModelRequest> requests;
    bool fail{false};
    bool throws{false};
    agent::Result<agent::ModelResponse> complete(const agent::ModelRequest& request) override {
        requests.push_back(request);
        if (throws) throw std::runtime_error("provider-secret-body");
        if (fail) return agent::Result<agent::ModelResponse>::failure(
            {agent::ErrorCode::HttpFailure, "provider-secret-body", true});
        return agent::Result<agent::ModelResponse>::success(response);
    }
    void text(std::string value) { response.content = {agent::TextBlock{std::move(value)}}; }
};

agent::MemoryConsolidationInput input() {
    return {"session-1", "E:/workspace", 2, 3, {
        {2, "task-2", {{agent::Role::User, {agent::TextBlock{"Use C++17 here"}}},
                        {agent::Role::Assistant, {agent::TextBlock{"Understood"}}}}},
        {3, "task-3", {{agent::Role::User, {agent::TextBlock{"Run tests"}}},
                        {agent::Role::Assistant, {agent::TextBlock{"Tests pass"}}}}}}, 5};
}
}

TEST_CASE(consolidator_requests_strict_json_and_keeps_exact_source_in_one_tool_free_message) {
    RecordingModel model;
    model.text(R"({"memories":[{"category":"constraint","scope":"workspace","content":"Use C++17"}]})");
    agent::ModelMemoryConsolidator consolidator(model, {5432});
    const auto result = consolidator.consolidate(input());
    REQUIRE(result.has_value());
    REQUIRE(result.value().size() == 1);
    REQUIRE(result.value()[0].category == agent::MemoryCategory::Constraint);
    REQUIRE(result.value()[0].scope_utf8 == "E:/workspace");
    REQUIRE(result.value()[0].content == "Use C++17");
    REQUIRE(model.requests.size() == 1);
    const auto& request = model.requests[0];
    REQUIRE(request.timeout_ms == 5432);
    REQUIRE(request.tools.empty());
    REQUIRE(request.evidence.items.empty());
    REQUIRE(request.messages.size() == 1);
    REQUIRE(request.messages[0].role == agent::Role::User);
    REQUIRE(request.messages[0].content.size() == 1);
    REQUIRE(request.system_prompt.find("untrusted") != std::string::npos);
    REQUIRE(request.system_prompt.find("JSON") != std::string::npos);
    const auto payload = nlohmann::json::parse(std::get<agent::TextBlock>(request.messages[0].content[0]).text);
    REQUIRE(payload["data_classification"] == "untrusted transcript");
    REQUIRE(payload["session_id"] == "session-1");
    REQUIRE(payload["workspace"] == "E:/workspace");
    REQUIRE(payload["source_turn_start"] == 2);
    REQUIRE(payload["source_turn_end"] == 3);
    REQUIRE(payload["max_candidates"] == 5);
    REQUIRE(payload["turns"].size() == 2);
    REQUIRE(payload["turns"][0]["turn_index"] == 2);
    REQUIRE(payload["turns"][1]["turn_index"] == 3);
    REQUIRE(payload["turns"][0]["messages"][0]["role"] == "user");
    REQUIRE(payload["turns"][0]["messages"][0]["content"][0]["text"] == "Use C++17 here");
}

TEST_CASE(consolidator_accepts_empty_array_and_all_known_categories) {
    RecordingModel model;
    agent::ModelMemoryConsolidator consolidator(model, {100});
    REQUIRE(consolidator.consolidate(input()).value().empty());
    model.response.stop_reason = agent::StopReason::StopSequence;
    model.text(R"({"memories":[{"category":"preference","scope":"workspace","content":"中文"},{"category":"decision","scope":"workspace","content":"d"},{"category":"fact","scope":"workspace","content":"f"},{"category":"workflow","scope":"workspace","content":"w"},{"category":"constraint","scope":"workspace","content":"c"}]})");
    const auto result = consolidator.consolidate(input());
    REQUIRE(result.has_value());
    REQUIRE(result.value().size() == 5);
    REQUIRE(result.value()[0].category == agent::MemoryCategory::Preference);
    REQUIRE(result.value()[1].category == agent::MemoryCategory::Decision);
    REQUIRE(result.value()[2].category == agent::MemoryCategory::Fact);
    REQUIRE(result.value()[3].category == agent::MemoryCategory::Workflow);
    REQUIRE(result.value()[4].category == agent::MemoryCategory::Constraint);
}

TEST_CASE(consolidator_conservatively_normalizes_all_model_global_scopes_to_workspace) {
    RecordingModel model;
    agent::ModelMemoryConsolidator consolidator(model, {100});
    for (const std::string category : {"preference", "fact"}) {
        model.text("{\"memories\":[{\"category\":\"" + category +
                   "\",\"scope\":\"global\",\"content\":\"Use C++17\"}]}");
        const auto result = consolidator.consolidate(input());
        REQUIRE(result.has_value());
        REQUIRE(result.value()[0].scope_utf8 == "E:/workspace");
    }
}

TEST_CASE(consolidator_leaves_secret_rejection_and_entry_byte_limits_to_policy) {
    RecordingModel model;
    agent::ModelMemoryConsolidator consolidator(model, {100});
    const std::string candidate = "api_key=provider-secret-body";
    model.text("{\"memories\":[{\"category\":\"fact\",\"scope\":\"workspace\",\"content\":\"" + candidate + "\"}]}");
    const auto result = consolidator.consolidate(input());
    REQUIRE(result.has_value());
    REQUIRE(result.value()[0].content == candidate);
    const agent::MemoryPolicy policy({4096, {}});
    REQUIRE(!policy.validate_candidate(result.value()[0].content).has_value());
    model.text(nlohmann::json{{"memories", {{{"category", "fact"}, {"scope", "workspace"}, {"content", std::string(4097, 'x')}}}}}.dump());
    const auto oversized = consolidator.consolidate(input());
    REQUIRE(oversized.has_value());
    REQUIRE(!policy.validate_candidate(oversized.value()[0].content).has_value());
}

TEST_CASE(consolidator_rejects_entire_malformed_json_candidate_set_with_sanitized_errors) {
    const std::vector<std::string> invalid{
        "", " ", "```json\n{\"memories\":[]}\n```", "prose {\"memories\":[]}",
        "{\"memories\":[]} prose", "{\"memories\":[]}{\"memories\":[]}", "[]", "null",
        "true", "0", "\"provider-secret-body\"", "{}", "{\"other\":[]}",
        R"({"memories":[],"other":1})", R"({"memories":{},"other":1})",
        R"({"memories":null})", R"({"memories":[1]})", R"({"memories":[[]]})",
        R"({"memories":[{}]})", R"({"memories":[{"category":"fact","scope":"workspace"}]})",
        R"({"memories":[{"scope":"workspace","content":"provider-secret-body"}]})",
        R"({"memories":[{"category":"fact","content":"provider-secret-body"}]})",
        R"({"memories":[{"category":"fact","scope":"workspace","content":"provider-secret-body","id":"x"}]})",
        R"({"memories":[{"category":"unknown","scope":"workspace","content":"provider-secret-body"}]})",
        R"({"memories":[{"category":"fact","scope":"unknown","content":"provider-secret-body"}]})",
        R"({"memories":[{"category":"fact","scope":"E:/other","content":"provider-secret-body"}]})",
        R"({"memories":[{"category":1,"scope":"workspace","content":"provider-secret-body"}]})",
        R"({"memories":[{"category":"fact","scope":false,"content":"provider-secret-body"}]})",
        R"({"memories":[{"category":"fact","scope":"workspace","content":null}]})",
        R"({"memories":[{"category":"fact","scope":"workspace","content":""}]})",
        R"({"memories":[{"category":"fact","scope":"workspace","content":" \t\n"}]})",
        R"({"memories":[{"category":"fact","scope":"workspace","content":"\u2003"}]})",
        R"({"memories":[{"category":"fact","scope":"workspace","content":"\u0000"}]})",
        R"({"memories":[{"category":"fact","scope":"workspace","content":"\ud800"}]})",
        R"({"memories":[{"category":"fact","scope":"workspace","content":"provider-secret-body"},null]})",
        R"({"memories":[],"memories":[]})",
        R"({"memories":[{"category":"fact","scope":"workspace","content":"provider-secret-body","content":"safe"}]})",
        R"({"memories":[{"category":"fact","scope":"workspace","content":"provider-secret-body","\u0063ontent":"safe"}]})",
        R"({"memories":[{"category":"bad","category":"fact","scope":"workspace","content":"safe"}]})",
        R"({"memories":[{"category":"fact","scope":"other","scope":"workspace","content":"safe"}]})",
        R"({"memories":[{"category":"fact","scope":"workspace","content":{"nested":{"x":1,"x":2}}}]})",
        R"({"memories":[],})", R"({/*comment*/"memories":[]})",
        std::string("{\"memories\":[{\"category\":\"fact\",\"scope\":\"workspace\",\"content\":\"") + "\xFF" + "\"}]}"};
    for (const auto& output : invalid) {
        RecordingModel model;
        model.text(output);
        agent::ModelMemoryConsolidator consolidator(model, {100});
        const auto result = consolidator.consolidate(input());
        REQUIRE(!result.has_value());
        REQUIRE(result.error().message.find("provider-secret-body") == std::string::npos);
    }
    RecordingModel model;
    model.text(R"({"memories":[{"category":"fact","scope":"workspace","content":"one"},{"category":"fact","scope":"workspace","content":"two"}]})");
    agent::ModelMemoryConsolidator consolidator(model, {100});
    auto limited = input();
    limited.max_candidates = 1;
    REQUIRE(!consolidator.consolidate(limited).has_value());
}

TEST_CASE(consolidator_rejects_nonterminal_nontext_and_provider_failures) {
    for (const auto reason : {agent::StopReason::Unknown, agent::StopReason::MaxTokens, agent::StopReason::ToolUse}) {
        RecordingModel model;
        model.response.stop_reason = reason;
        agent::ModelMemoryConsolidator consolidator(model, {100});
        REQUIRE(!consolidator.consolidate(input()).has_value());
    }
    const std::vector<std::vector<agent::ContentBlock>> invalid{
        {}, {agent::TextBlock{""}}, {agent::TextBlock{R"({"memories":[]})"}, agent::TextBlock{"extra"}},
        {agent::ToolUseBlock{{"id", "tool", agent::Value::object({})}}},
        {agent::ToolResultBlock{{"id", "provider-secret-body", false}}}};
    for (const auto& blocks : invalid) {
        RecordingModel model;
        model.response.content = blocks;
        agent::ModelMemoryConsolidator consolidator(model, {100});
        REQUIRE(!consolidator.consolidate(input()).has_value());
    }
    for (const bool throws : {false, true}) {
        RecordingModel model;
        model.fail = true;
        model.throws = throws;
        agent::ModelMemoryConsolidator consolidator(model, {100});
        const auto result = consolidator.consolidate(input());
        REQUIRE(!result.has_value());
        REQUIRE(result.error().message.find("provider-secret-body") == std::string::npos);
    }
}

TEST_CASE(consolidator_rejects_invalid_source_range_and_input_before_calling_model) {
    using Mutate = std::function<void(agent::MemoryConsolidationInput&)>;
    const std::vector<Mutate> mutations{
        [](auto& i) { i.session_id.clear(); }, [](auto& i) { i.session_id = "\xFF"; },
        [](auto& i) { i.workspace_utf8.clear(); }, [](auto& i) { i.workspace_utf8 = "\xFF"; },
        [](auto& i) { i.turns.clear(); }, [](auto& i) { i.source_turn_start = 0; },
        [](auto& i) { i.source_turn_start = 3; }, [](auto& i) { i.source_turn_end = 2; },
        [](auto& i) { i.source_turn_start = 4; }, [](auto& i) { i.max_candidates = 0; },
        [](auto& i) { i.source_turn_end = 0; },
        [](auto& i) { i.turns[1].turn_index = 2; },
        [](auto& i) { i.turns[1].turn_index = 1; },
        [](auto& i) { i.turns[1].turn_index = 4; },
        [](auto& i) { i.turns[0].messages[0].role = agent::Role::Assistant; },
        [](auto& i) { std::get<agent::TextBlock>(i.turns[0].messages[0].content[0]).text = "\xFF"; }};
    for (const auto& mutate : mutations) {
        auto request = input();
        mutate(request);
        RecordingModel model;
        agent::ModelMemoryConsolidator consolidator(model, {100});
        REQUIRE(!consolidator.consolidate(request).has_value());
        REQUIRE(model.requests.empty());
    }
    for (const std::int64_t timeout : {0, -1}) {
        RecordingModel model;
        agent::ModelMemoryConsolidator consolidator(model, {timeout});
        REQUIRE(!consolidator.consolidate(input()).has_value());
        REQUIRE(model.requests.empty());
    }
}

TEST_CASE(consolidator_accepts_failed_turn_gaps_inside_inclusive_source_range) {
    auto request = input();
    request.source_turn_start = 1;
    request.source_turn_end = 4;
    request.turns[0].turn_index = 1;
    request.turns[1].turn_index = 3;
    RecordingModel model;
    agent::ModelMemoryConsolidator consolidator(model, {100});
    const auto result = consolidator.consolidate(request);
    REQUIRE(result.has_value());
    REQUIRE(result.value().empty());
    REQUIRE(model.requests.size() == 1);
    const auto payload = nlohmann::json::parse(
        std::get<agent::TextBlock>(model.requests[0].messages[0].content[0]).text);
    REQUIRE(payload["source_turn_start"] == 1);
    REQUIRE(payload["source_turn_end"] == 4);
    REQUIRE(payload["turns"].size() == 2);
    REQUIRE(payload["turns"][0]["turn_index"] == 1);
    REQUIRE(payload["turns"][1]["turn_index"] == 3);
}

TEST_CASE(consolidator_accepts_failed_turns_at_both_source_range_endpoints) {
    auto request = input();
    request.source_turn_start = 1;
    request.source_turn_end = 4;
    RecordingModel model;
    agent::ModelMemoryConsolidator consolidator(model, {100});
    REQUIRE(consolidator.consolidate(request).has_value());
    REQUIRE(model.requests.size() == 1);
}

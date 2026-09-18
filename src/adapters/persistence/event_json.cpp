#include "adapters/persistence/event_json.h"

#include "adapters/json/value_json.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace agent {
namespace {

using Json = nlohmann::json;

class DecodeError : public std::runtime_error {
public:
    explicit DecodeError(const char* message) : std::runtime_error(message) {}
};

void require_object(const Json& json) {
    if (!json.is_object()) {
        throw DecodeError("expected object");
    }
}

void require_array(const Json& json) {
    if (!json.is_array()) {
        throw DecodeError("expected array");
    }
}

void require_exact_keys(
    const Json& json,
    std::initializer_list<std::string_view> expected_keys) {
    require_object(json);
    if (json.size() != expected_keys.size()) {
        throw DecodeError("unexpected object keys");
    }
    for (const auto key : expected_keys) {
        if (!json.contains(std::string(key))) {
            throw DecodeError("missing object key");
        }
    }
}

template <typename Integer>
Integer unsigned_integer(const Json& json) {
    if (!json.is_number_unsigned() && !json.is_number_integer()) {
        throw DecodeError("expected unsigned integer");
    }
    if (!json.is_number_unsigned() && json.is_number_integer() &&
        json.get<std::int64_t>() < 0) {
        throw DecodeError("expected nonnegative integer");
    }
    const auto value = json.get<std::uint64_t>();
    if (value > static_cast<std::uint64_t>(std::numeric_limits<Integer>::max())) {
        throw DecodeError("unsigned integer out of range");
    }
    return static_cast<Integer>(value);
}

std::int64_t signed_integer(const Json& json) {
    if (json.is_number_unsigned()) {
        const auto value = json.get<std::uint64_t>();
        if (value > static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max())) {
            throw DecodeError("signed integer out of range");
        }
        return static_cast<std::int64_t>(value);
    }
    if (!json.is_number_integer()) {
        throw DecodeError("expected integer");
    }
    return json.get<std::int64_t>();
}

std::string required_string(const Json& json, const char* key) {
    require_object(json);
    const auto& value = json.at(key);
    if (!value.is_string()) {
        throw DecodeError("expected string");
    }
    return value.get<std::string>();
}

bool required_bool(const Json& json, const char* key) {
    require_object(json);
    const auto& value = json.at(key);
    if (!value.is_boolean()) {
        throw DecodeError("expected boolean");
    }
    return value.get<bool>();
}

const char* error_code_name(ErrorCode code) {
    switch (code) {
    case ErrorCode::InvalidInput: return "invalid_input";
    case ErrorCode::InvalidConfiguration: return "invalid_configuration";
    case ErrorCode::PersistenceFailure: return "persistence_failure";
    case ErrorCode::TransportFailure: return "transport_failure";
    case ErrorCode::RequestTimeout: return "request_timeout";
    case ErrorCode::HttpFailure: return "http_failure";
    case ErrorCode::ProtocolFailure: return "protocol_failure";
    case ErrorCode::DependencyUnavailable: return "dependency_unavailable";
    case ErrorCode::InvalidTransition: return "invalid_transition";
    case ErrorCode::BudgetExceeded: return "budget_exceeded";
    case ErrorCode::Cancelled: return "cancelled";
    }
    throw std::invalid_argument("unknown error code");
}

ErrorCode error_code_from_name(const std::string& name) {
    if (name == "invalid_input") return ErrorCode::InvalidInput;
    if (name == "invalid_configuration") return ErrorCode::InvalidConfiguration;
    if (name == "persistence_failure") return ErrorCode::PersistenceFailure;
    if (name == "transport_failure") return ErrorCode::TransportFailure;
    if (name == "request_timeout") return ErrorCode::RequestTimeout;
    if (name == "http_failure") return ErrorCode::HttpFailure;
    if (name == "protocol_failure") return ErrorCode::ProtocolFailure;
    if (name == "dependency_unavailable") return ErrorCode::DependencyUnavailable;
    if (name == "invalid_transition") return ErrorCode::InvalidTransition;
    if (name == "budget_exceeded") return ErrorCode::BudgetExceeded;
    if (name == "cancelled") return ErrorCode::Cancelled;
    throw DecodeError("unknown error code");
}

Json runtime_error_to_json(const RuntimeError& error) {
    return {{"code", error_code_name(error.code)},
            {"message", error.message},
            {"retryable", error.retryable}};
}

RuntimeError runtime_error_from_json(const Json& json) {
    require_exact_keys(json, {"code", "message", "retryable"});
    return {error_code_from_name(required_string(json, "code")),
            required_string(json, "message"),
            required_bool(json, "retryable")};
}

const char* role_name(Role role) {
    switch (role) {
    case Role::System: return "system";
    case Role::User: return "user";
    case Role::Assistant: return "assistant";
    }
    throw std::invalid_argument("unknown role");
}

Role role_from_name(const std::string& name) {
    if (name == "system") return Role::System;
    if (name == "user") return Role::User;
    if (name == "assistant") return Role::Assistant;
    throw DecodeError("unknown role");
}

const char* stop_reason_name(StopReason reason) {
    switch (reason) {
    case StopReason::EndTurn: return "end_turn";
    case StopReason::ToolUse: return "tool_use";
    case StopReason::MaxTokens: return "max_tokens";
    case StopReason::StopSequence: return "stop_sequence";
    case StopReason::Unknown: return "unknown";
    }
    throw std::invalid_argument("unknown stop reason");
}

StopReason stop_reason_from_name(const std::string& name) {
    if (name == "end_turn") return StopReason::EndTurn;
    if (name == "tool_use") return StopReason::ToolUse;
    if (name == "max_tokens") return StopReason::MaxTokens;
    if (name == "stop_sequence") return StopReason::StopSequence;
    if (name == "unknown") return StopReason::Unknown;
    throw DecodeError("unknown stop reason");
}

Json tool_definition_to_json(const ToolDefinition& definition) {
    return {{"name", definition.name},
            {"description", definition.description},
            {"input_schema", value_to_json(definition.input_schema)}};
}

ToolDefinition tool_definition_from_json(const Json& json) {
    require_exact_keys(json, {"name", "description", "input_schema"});
    auto schema = value_from_json(json.at("input_schema"));
    if (!schema.has_value()) {
        throw DecodeError("invalid tool input schema");
    }
    return {required_string(json, "name"), required_string(json, "description"),
            std::move(schema.value())};
}

Json tool_call_to_json(const ToolCall& call) {
    return {{"id", call.id},
            {"name", call.name},
            {"arguments", value_to_json(call.arguments)}};
}

ToolCall tool_call_from_json(const Json& json) {
    require_exact_keys(json, {"id", "name", "arguments"});
    auto arguments = value_from_json(json.at("arguments"));
    if (!arguments.has_value()) {
        throw DecodeError("invalid tool call arguments");
    }
    return {required_string(json, "id"), required_string(json, "name"),
            std::move(arguments.value())};
}

Json tool_result_to_json(const ToolResult& result) {
    return {{"tool_call_id", result.tool_call_id},
            {"content", result.content},
            {"is_error", result.is_error}};
}

ToolResult tool_result_from_json(const Json& json) {
    require_exact_keys(json, {"tool_call_id", "content", "is_error"});
    return {required_string(json, "tool_call_id"),
            required_string(json, "content"), required_bool(json, "is_error")};
}

Json content_block_to_json(const ContentBlock& block) {
    return std::visit(
        [](const auto& typed) -> Json {
            using Block = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<Block, TextBlock>) {
                return {{"type", "text"}, {"text", typed.text}};
            } else if constexpr (std::is_same_v<Block, ToolUseBlock>) {
                return {{"type", "tool_use"}, {"call", tool_call_to_json(typed.call)}};
            } else if constexpr (std::is_same_v<Block, ToolResultBlock>) {
                return {{"type", "tool_result"},
                        {"result", tool_result_to_json(typed.result)}};
            } else {
                // T25: CompactRequestBlock round-trips as {type, reason}.
                return {{"type", "compact_request"}, {"reason", typed.reason}};
            }
        },
        block);
}

ContentBlock content_block_from_json(const Json& json) {
    require_object(json);
    const auto type = required_string(json, "type");
    if (type == "text") {
        require_exact_keys(json, {"type", "text"});
        return TextBlock{required_string(json, "text")};
    }
    if (type == "tool_use") {
        require_exact_keys(json, {"type", "call"});
        return ToolUseBlock{tool_call_from_json(json.at("call"))};
    }
    if (type == "tool_result") {
        require_exact_keys(json, {"type", "result"});
        return ToolResultBlock{tool_result_from_json(json.at("result"))};
    }
    if (type == "compact_request") {
        // T25: missing reason is allowed and yields an empty string.
        std::string reason;
        if (json.contains("reason") && json.at("reason").is_string()) {
            reason = json.at("reason").get<std::string>();
        }
        return CompactRequestBlock{std::move(reason)};
    }
    throw DecodeError("unknown content block type");
}

Json content_to_json(const std::vector<ContentBlock>& content) {
    auto json = Json::array();
    for (const auto& block : content) {
        json.push_back(content_block_to_json(block));
    }
    return json;
}

std::vector<ContentBlock> content_from_json(const Json& json) {
    require_array(json);
    std::vector<ContentBlock> content;
    content.reserve(json.size());
    for (const auto& block : json) {
        content.push_back(content_block_from_json(block));
    }
    return content;
}

Json message_to_json(const Message& message) {
    return {{"role", role_name(message.role)},
            {"content", content_to_json(message.content)}};
}

Message message_from_json(const Json& json);

Json messages_to_json(const std::vector<Message>& messages) {
    auto json = Json::array();
    for (const auto& message : messages) {
        json.push_back(message_to_json(message));
    }
    return json;
}

std::vector<Message> messages_from_json(const Json& json) {
    require_array(json);
    std::vector<Message> messages;
    messages.reserve(json.size());
    for (const auto& message : json) {
        messages.push_back(message_from_json(message));
    }
    return messages;
}

Message message_from_json(const Json& json) {
    require_exact_keys(json, {"role", "content"});
    return {role_from_name(required_string(json, "role")),
            content_from_json(json.at("content"))};
}

Json evidence_pack_to_json(const EvidencePack& evidence) {
    auto items = Json::array();
    for (const auto& item : evidence.items) {
        items.push_back({{"source_id", item.source_id},
                         {"content", item.content},
                         {"metadata", value_to_json(item.metadata)}});
    }
    return {{"items", std::move(items)},
            {"authoritative_no_match", evidence.authoritative_no_match},
            {"tool_use_forbidden", evidence.tool_use_forbidden}};
}

EvidencePack evidence_pack_from_json(const Json& json) {
    const bool has_outcome = json.contains("authoritative_no_match");
    const bool has_tool_policy = json.contains("tool_use_forbidden");
    if (has_outcome && has_tool_policy) {
        require_exact_keys(
            json, {"items", "authoritative_no_match", "tool_use_forbidden"});
    } else if (has_outcome) {
        // Logs written after explicit no-match and before the tool policy bit
        // remain replayable with their historical behavior.
        require_exact_keys(json, {"items", "authoritative_no_match"});
    } else {
        // Schema-v1 logs written before the explicit no-match outcome remain
        // replayable. Their evidence semantics were items-only.
        require_exact_keys(json, {"items"});
    }
    const auto& items_json = json.at("items");
    require_array(items_json);
    EvidencePack pack;
    pack.authoritative_no_match =
        has_outcome && required_bool(json, "authoritative_no_match");
    pack.tool_use_forbidden =
        has_tool_policy && required_bool(json, "tool_use_forbidden");
    pack.items.reserve(items_json.size());
    for (const auto& item : items_json) {
        require_exact_keys(item, {"source_id", "content", "metadata"});
        auto metadata = value_from_json(item.at("metadata"));
        if (!metadata.has_value()) {
            throw DecodeError("invalid evidence metadata");
        }
        pack.items.push_back({required_string(item, "source_id"),
                              required_string(item, "content"),
                              std::move(metadata.value())});
    }
    return pack;
}

Json model_request_to_json(const ModelRequest& request) {
    auto tools = Json::array();
    for (const auto& tool : request.tools) {
        tools.push_back(tool_definition_to_json(tool));
    }
    return {{"system_prompt", request.system_prompt},
            {"messages", messages_to_json(request.messages)},
            {"tools", std::move(tools)},
            {"timeout_ms", request.timeout_ms},
            {"evidence", evidence_pack_to_json(request.evidence)}};
}

ModelRequest model_request_from_json(const Json& json) {
    require_exact_keys(
        json, {"system_prompt", "messages", "tools", "timeout_ms", "evidence"});
    const auto& messages_json = json.at("messages");
    const auto& tools_json = json.at("tools");
    require_array(messages_json);
    require_array(tools_json);
    ModelRequest request;
    request.system_prompt = required_string(json, "system_prompt");
    request.timeout_ms = signed_integer(json.at("timeout_ms"));
    if (request.timeout_ms <= 0) {
        throw DecodeError("model timeout must be positive");
    }
    request.evidence = evidence_pack_from_json(json.at("evidence"));
    request.messages.reserve(messages_json.size());
    for (const auto& message : messages_json) {
        request.messages.push_back(message_from_json(message));
    }
    request.tools.reserve(tools_json.size());
    for (const auto& tool : tools_json) {
        request.tools.push_back(tool_definition_from_json(tool));
    }
    return request;
}

Json model_response_to_json(const ModelResponse& response) {
    return {{"content", content_to_json(response.content)},
            {"stop_reason", stop_reason_name(response.stop_reason)},
            {"raw_stop_reason", response.raw_stop_reason},
            {"input_tokens", response.input_tokens},
            {"output_tokens", response.output_tokens},
            {"provider_request_id", response.provider_request_id}};
}

ModelResponse model_response_from_json(const Json& json) {
    require_exact_keys(
        json, {"content", "stop_reason", "raw_stop_reason", "input_tokens",
               "output_tokens", "provider_request_id"});
    return {content_from_json(json.at("content")),
            stop_reason_from_name(required_string(json, "stop_reason")),
            required_string(json, "raw_stop_reason"),
            unsigned_integer<std::size_t>(json.at("input_tokens")),
            unsigned_integer<std::size_t>(json.at("output_tokens")),
            required_string(json, "provider_request_id")};
}

Json budgets_to_json(const RuntimeBudgets& budgets) {
    return {{"max_model_rounds", budgets.max_model_rounds},
            {"max_tool_calls", budgets.max_tool_calls},
            {"max_task_time_ms", budgets.max_task_time_ms},
            {"model_timeout_ms", budgets.model_timeout_ms}};
}

RuntimeBudgets budgets_from_json(const Json& json) {
    require_exact_keys(
        json, {"max_model_rounds", "max_tool_calls", "max_task_time_ms",
               "model_timeout_ms"});
    RuntimeBudgets budgets{
        unsigned_integer<std::size_t>(json.at("max_model_rounds")),
        unsigned_integer<std::size_t>(json.at("max_tool_calls")),
        signed_integer(json.at("max_task_time_ms")),
        signed_integer(json.at("model_timeout_ms"))};
    if (!has_positive_runtime_budgets(budgets)) {
        throw DecodeError("runtime budgets must be positive");
    }
    return budgets;
}

const char* event_kind_name(EventKind kind) {
    switch (kind) {
    case EventKind::TaskStarted: return "task_started";
    case EventKind::ContextPreparationStarted: return "context_preparation_started";
    case EventKind::ContextPrepared: return "context_prepared";
    case EventKind::KnowledgeNoMatch: return "knowledge_no_match";
    case EventKind::ContextPreparationFailed: return "context_preparation_failed";
    case EventKind::ModelCallStarted: return "model_call_started";
    case EventKind::ModelCallSucceeded: return "model_call_succeeded";
    case EventKind::ModelCallFailed: return "model_call_failed";
    case EventKind::ToolCallStarted: return "tool_call_started";
    case EventKind::ToolCallSucceeded: return "tool_call_succeeded";
    case EventKind::ToolCallFailed: return "tool_call_failed";
    case EventKind::TaskCompleted: return "task_completed";
    case EventKind::TaskFailed: return "task_failed";
    case EventKind::TaskBudgetExceeded: return "task_budget_exceeded";
    case EventKind::TaskCancelled: return "task_cancelled";
    }
    throw std::invalid_argument("unknown event kind");
}

Json payload_to_json(const EventPayload& payload) {
    return std::visit(
        [](const auto& typed) -> Json {
            using Payload = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<Payload, TaskStartedPayload>) {
                Json json{{"issue", typed.issue},
                          {"workspace_utf8", typed.workspace_utf8},
                          {"budgets", budgets_to_json(typed.budgets)}};
                if (!typed.initial_messages.empty()) {
                    json["initial_messages"] =
                        messages_to_json(typed.initial_messages);
                }
                if (typed.session_link.has_value()) {
                    json["session_link"] = {
                        {"session_id", typed.session_link->session_id},
                        {"turn_index", typed.session_link->turn_index}};
                }
                return json;
            } else if constexpr (
                std::is_same_v<Payload, ContextPreparationStartedPayload>) {
                return Json::object();
            } else if constexpr (std::is_same_v<Payload, ContextPreparedPayload>) {
                return {{"evidence", evidence_pack_to_json(typed.evidence)}};
            } else if constexpr (std::is_same_v<Payload, KnowledgeNoMatchPayload>) {
                return {{"final_text", typed.final_text}};
            } else if constexpr (
                std::is_same_v<Payload, ContextPreparationFailedPayload> ||
                std::is_same_v<Payload, ModelCallFailedPayload> ||
                std::is_same_v<Payload, TaskFailedPayload>) {
                return {{"error", runtime_error_to_json(typed.error)}};
            } else if constexpr (std::is_same_v<Payload, ModelCallStartedPayload>) {
                return {{"request", model_request_to_json(typed.request)}};
            } else if constexpr (
                std::is_same_v<Payload, ModelCallSucceededPayload>) {
                return {{"response", model_response_to_json(typed.response)}};
            } else if constexpr (std::is_same_v<Payload, ToolCallStartedPayload>) {
                return {{"call", tool_call_to_json(typed.call)}};
            } else if constexpr (
                std::is_same_v<Payload, ToolCallSucceededPayload>) {
                return {{"result", tool_result_to_json(typed.result)}};
            } else if constexpr (std::is_same_v<Payload, ToolCallFailedPayload>) {
                return {{"tool_call_id", typed.tool_call_id},
                        {"error", runtime_error_to_json(typed.error)}};
            } else if constexpr (std::is_same_v<Payload, TaskCompletedPayload>) {
                return {{"final_text", typed.final_text}};
            } else if constexpr (
                std::is_same_v<Payload, TaskBudgetExceededPayload>) {
                return {{"budget_name", typed.budget_name},
                        {"error", runtime_error_to_json(typed.error)}};
            } else {
                return {{"reason", typed.reason},
                        {"error", runtime_error_to_json(typed.error)}};
            }
        },
        payload);
}

EventPayload payload_from_json(const std::string& type, const Json& json) {
    require_object(json);
    if (type == "task_started") {
        const bool has_messages = json.contains("initial_messages");
        const bool has_link = json.contains("session_link");
        if (has_messages && has_link) {
            require_exact_keys(json, {"issue", "workspace_utf8", "budgets",
                                      "initial_messages", "session_link"});
        } else if (has_messages) {
            require_exact_keys(json, {"issue", "workspace_utf8", "budgets",
                                      "initial_messages"});
        } else if (has_link) {
            require_exact_keys(json, {"issue", "workspace_utf8", "budgets",
                                      "session_link"});
        } else {
            require_exact_keys(json, {"issue", "workspace_utf8", "budgets"});
        }
        TaskStartedPayload payload{
            required_string(json, "issue"),
            required_string(json, "workspace_utf8"),
            budgets_from_json(json.at("budgets"))};
        if (has_messages) {
            payload.initial_messages =
                messages_from_json(json.at("initial_messages"));
        }
        if (has_link) {
            const auto& link = json.at("session_link");
            require_exact_keys(link, {"session_id", "turn_index"});
            payload.session_link = SessionTaskLink{
                required_string(link, "session_id"),
                unsigned_integer<std::uint64_t>(link.at("turn_index"))};
        }
        return payload;
    }
    if (type == "context_preparation_started") {
        require_exact_keys(json, {});
        return ContextPreparationStartedPayload{};
    }
    if (type == "context_prepared") {
        require_exact_keys(json, {"evidence"});
        return ContextPreparedPayload{evidence_pack_from_json(json.at("evidence"))};
    }
    if (type == "knowledge_no_match") {
        require_exact_keys(json, {"final_text"});
        return KnowledgeNoMatchPayload{required_string(json, "final_text")};
    }
    if (type == "context_preparation_failed") {
        require_exact_keys(json, {"error"});
        return ContextPreparationFailedPayload{
            runtime_error_from_json(json.at("error"))};
    }
    if (type == "model_call_started") {
        require_exact_keys(json, {"request"});
        return ModelCallStartedPayload{model_request_from_json(json.at("request"))};
    }
    if (type == "model_call_succeeded") {
        require_exact_keys(json, {"response"});
        return ModelCallSucceededPayload{
            model_response_from_json(json.at("response"))};
    }
    if (type == "model_call_failed") {
        require_exact_keys(json, {"error"});
        return ModelCallFailedPayload{runtime_error_from_json(json.at("error"))};
    }
    if (type == "tool_call_started") {
        require_exact_keys(json, {"call"});
        return ToolCallStartedPayload{tool_call_from_json(json.at("call"))};
    }
    if (type == "tool_call_succeeded") {
        require_exact_keys(json, {"result"});
        return ToolCallSucceededPayload{tool_result_from_json(json.at("result"))};
    }
    if (type == "tool_call_failed") {
        require_exact_keys(json, {"tool_call_id", "error"});
        return ToolCallFailedPayload{required_string(json, "tool_call_id"),
                                     runtime_error_from_json(json.at("error"))};
    }
    if (type == "task_completed") {
        require_exact_keys(json, {"final_text"});
        return TaskCompletedPayload{required_string(json, "final_text")};
    }
    if (type == "task_failed") {
        require_exact_keys(json, {"error"});
        return TaskFailedPayload{runtime_error_from_json(json.at("error"))};
    }
    if (type == "task_budget_exceeded") {
        require_exact_keys(json, {"budget_name", "error"});
        return TaskBudgetExceededPayload{
            required_string(json, "budget_name"),
            runtime_error_from_json(json.at("error"))};
    }
    if (type == "task_cancelled") {
        require_exact_keys(json, {"reason", "error"});
        return TaskCancelledPayload{required_string(json, "reason"),
                                    runtime_error_from_json(json.at("error"))};
    }
    throw DecodeError("unknown event type");
}

}  // namespace

nlohmann::json message_list_to_json(const std::vector<Message>& messages) {
    return messages_to_json(messages);
}

Result<std::vector<Message>> message_list_from_json(
    const nlohmann::json& json) {
    try {
        auto messages = messages_from_json(json);
        if (!conversation_history_is_valid(messages)) {
            throw DecodeError("invalid conversation history");
        }
        return Result<std::vector<Message>>::success(std::move(messages));
    } catch (const std::exception&) {
        return Result<std::vector<Message>>::failure(
            {ErrorCode::PersistenceFailure,
             "message list validation failed", false});
    }
}

nlohmann::json event_to_json(const RuntimeEvent& event) {
    return {{"schema_version", event.schema_version},
            {"sequence", event.sequence},
            {"task_id", event.task_id},
            {"timestamp", event.timestamp_utc},
            {"event_type", event_kind_name(event_kind(event.payload))},
            {"correlation_id", event.correlation_id},
            {"payload", payload_to_json(event.payload)}};
}

Result<RuntimeEvent> event_from_json(const nlohmann::json& json) {
    try {
        require_exact_keys(
            json, {"schema_version", "sequence", "task_id", "timestamp",
                   "event_type", "correlation_id", "payload"});
        const auto schema_version =
            unsigned_integer<std::uint32_t>(json.at("schema_version"));
        if (schema_version != 1) {
            throw DecodeError("unsupported schema version");
        }
        RuntimeEvent event;
        event.schema_version = schema_version;
        event.sequence = unsigned_integer<std::uint64_t>(json.at("sequence"));
        event.task_id = required_string(json, "task_id");
        event.timestamp_utc = required_string(json, "timestamp");
        event.correlation_id = required_string(json, "correlation_id");
        event.payload = payload_from_json(required_string(json, "event_type"),
                                          json.at("payload"));
        return Result<RuntimeEvent>::success(std::move(event));
    } catch (const std::exception&) {
        return Result<RuntimeEvent>::failure(
            {ErrorCode::PersistenceFailure, "invalid persisted event", false});
    }
}

}  // namespace agent

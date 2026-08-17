#pragma once

#include "domain/runtime_error.h"
#include "domain/value.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace agent {

inline bool operator==(const RuntimeError& left, const RuntimeError& right) {
    return left.code == right.code && left.message == right.message &&
           left.retryable == right.retryable;
}

enum class Role { System, User, Assistant };

struct TextBlock {
    std::string text;
};

struct ToolDefinition {
    std::string name;
    std::string description;
    Value input_schema;
};

struct ToolCall {
    std::string id;
    std::string name;
    Value arguments;
};

struct ToolResult {
    std::string tool_call_id;
    std::string content;
    bool is_error{false};
};

struct ToolUseBlock {
    ToolCall call;
};

struct ToolResultBlock {
    ToolResult result;
};

using ContentBlock = std::variant<TextBlock, ToolUseBlock, ToolResultBlock>;

struct Message {
    Role role;
    std::vector<ContentBlock> content;
};

struct Evidence {
    std::string source_id;
    std::string content;
    Value metadata;
};

struct EvidencePack {
    std::vector<Evidence> items;
};

struct ModelRequest {
    std::string system_prompt;
    std::vector<Message> messages;
    std::vector<ToolDefinition> tools;
    std::int64_t timeout_ms{120'000};
    EvidencePack evidence;
};

enum class StopReason { EndTurn, ToolUse, MaxTokens, StopSequence, Unknown };

struct ModelResponse {
    std::vector<ContentBlock> content;
    StopReason stop_reason{StopReason::Unknown};
    std::string raw_stop_reason;
    std::size_t input_tokens{0};
    std::size_t output_tokens{0};
    std::string provider_request_id;
};

inline bool operator==(const TextBlock& left, const TextBlock& right) {
    return left.text == right.text;
}

inline bool operator==(const ToolDefinition& left, const ToolDefinition& right) {
    return left.name == right.name && left.description == right.description &&
           left.input_schema == right.input_schema;
}

inline bool operator==(const ToolCall& left, const ToolCall& right) {
    return left.id == right.id && left.name == right.name &&
           left.arguments == right.arguments;
}

inline bool operator==(const ToolResult& left, const ToolResult& right) {
    return left.tool_call_id == right.tool_call_id && left.content == right.content &&
           left.is_error == right.is_error;
}

inline bool operator==(const ToolUseBlock& left, const ToolUseBlock& right) {
    return left.call == right.call;
}

inline bool operator==(const ToolResultBlock& left, const ToolResultBlock& right) {
    return left.result == right.result;
}

inline bool operator==(const Message& left, const Message& right) {
    return left.role == right.role && left.content == right.content;
}

inline bool operator==(const Evidence& left, const Evidence& right) {
    return left.source_id == right.source_id && left.content == right.content &&
           left.metadata == right.metadata;
}

inline bool operator==(const EvidencePack& left, const EvidencePack& right) {
    return left.items == right.items;
}

inline bool operator==(const ModelRequest& left, const ModelRequest& right) {
    return left.system_prompt == right.system_prompt && left.messages == right.messages &&
           left.tools == right.tools && left.timeout_ms == right.timeout_ms &&
           left.evidence == right.evidence;
}

inline bool operator==(const ModelResponse& left, const ModelResponse& right) {
    return left.content == right.content && left.stop_reason == right.stop_reason &&
           left.raw_stop_reason == right.raw_stop_reason &&
           left.input_tokens == right.input_tokens &&
           left.output_tokens == right.output_tokens &&
           left.provider_request_id == right.provider_request_id;
}

}  // namespace agent

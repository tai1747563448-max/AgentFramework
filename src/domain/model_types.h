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

// T25: marker block the model emits when it thinks the conversation
// needs compaction now. The runtime recognises this in the response and
// invokes the reactive compact stage before continuing the loop.
struct CompactRequestBlock {
    std::string reason;
};

using ContentBlock = std::variant<TextBlock, ToolUseBlock, ToolResultBlock,
                                  CompactRequestBlock>;

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
    bool authoritative_no_match{false};
    bool tool_use_forbidden{false};
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
    // T12 (v2 §3): provider-neutral StopReason enum only. Adapters
    // map their provider-specific string through ports/stop_reason_codec.h
    // internally; the runtime no longer sees raw provider tokens.
    // The former is_known_stop_reason_pair(raw, ...) helper moved to
    // ports/stop_reason_codec.h::is_known_stop_reason(reason) so the
    // runtime's check stays provider-neutral.
    StopReason stop_reason{StopReason::Unknown};
    std::size_t input_tokens{0};
    std::size_t output_tokens{0};
    std::string provider_request_id;
};

inline bool response_text_blocks_are_valid(const ModelResponse& response) {
    for (const auto& block : response.content) {
        const auto* text = std::get_if<TextBlock>(&block);
        if (text != nullptr && text->text.empty() &&
            response.stop_reason != StopReason::MaxTokens) {
            return false;
        }
    }
    return true;
}

inline bool response_tool_uses_are_valid(const ModelResponse& response) {
    std::vector<std::string> tool_call_ids;
    for (const auto& block : response.content) {
        const auto* tool_use = std::get_if<ToolUseBlock>(&block);
        if (tool_use == nullptr) {
            continue;
        }
        const auto& call = tool_use->call;
        if (call.id.empty() || call.name.empty() || !call.arguments.is_object()) {
            return false;
        }
        for (const auto& accepted_id : tool_call_ids) {
            if (accepted_id == call.id) {
                return false;
            }
        }
        tool_call_ids.push_back(call.id);
    }
    return true;
}

inline bool conversation_history_is_valid(
    const std::vector<Message>& messages) {
    if (messages.empty()) {
        return true;
    }

    std::vector<std::string> pending_tool_ids;
    Role expected_role = Role::User;
    for (const auto& message : messages) {
        if (message.role != expected_role || message.content.empty() ||
            message.role == Role::System) {
            return false;
        }

        if (message.role == Role::Assistant) {
            if (!pending_tool_ids.empty()) {
                return false;
            }
            for (const auto& block : message.content) {
                if (const auto* text = std::get_if<TextBlock>(&block)) {
                    if (text->text.empty()) {
                        return false;
                    }
                    continue;
                }
                const auto* tool = std::get_if<ToolUseBlock>(&block);
                if (tool == nullptr || tool->call.id.empty() ||
                    tool->call.name.empty() ||
                    !tool->call.arguments.is_object()) {
                    return false;
                }
                for (const auto& accepted : pending_tool_ids) {
                    if (accepted == tool->call.id) {
                        return false;
                    }
                }
                pending_tool_ids.push_back(tool->call.id);
            }
            expected_role = Role::User;
            continue;
        }

        if (pending_tool_ids.empty()) {
            for (const auto& block : message.content) {
                const auto* text = std::get_if<TextBlock>(&block);
                if (text == nullptr || text->text.empty()) {
                    return false;
                }
            }
        } else {
            std::vector<std::string> returned_ids;
            for (const auto& block : message.content) {
                const auto* result = std::get_if<ToolResultBlock>(&block);
                if (result == nullptr || result->result.tool_call_id.empty()) {
                    return false;
                }
                bool matched = false;
                for (const auto& pending : pending_tool_ids) {
                    if (pending == result->result.tool_call_id) {
                        matched = true;
                        break;
                    }
                }
                for (const auto& returned : returned_ids) {
                    if (returned == result->result.tool_call_id) {
                        return false;
                    }
                }
                if (!matched) {
                    return false;
                }
                returned_ids.push_back(result->result.tool_call_id);
            }
            if (returned_ids.size() != pending_tool_ids.size()) {
                return false;
            }
            pending_tool_ids.clear();
        }
        expected_role = Role::Assistant;
    }

    return pending_tool_ids.empty() && expected_role == Role::User;
}

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

inline bool operator==(const CompactRequestBlock& left,
                       const CompactRequestBlock& right) {
    return left.reason == right.reason;
}

inline bool operator==(const Message& left, const Message& right) {
    return left.role == right.role && left.content == right.content;
}

inline bool operator==(const Evidence& left, const Evidence& right) {
    return left.source_id == right.source_id && left.content == right.content &&
           left.metadata == right.metadata;
}

inline bool operator==(const EvidencePack& left, const EvidencePack& right) {
    return left.items == right.items &&
           left.authoritative_no_match == right.authoritative_no_match &&
           left.tool_use_forbidden == right.tool_use_forbidden;
}

inline bool operator==(const ModelRequest& left, const ModelRequest& right) {
    return left.system_prompt == right.system_prompt && left.messages == right.messages &&
           left.tools == right.tools && left.timeout_ms == right.timeout_ms &&
           left.evidence == right.evidence;
}

inline bool operator==(const ModelResponse& left, const ModelResponse& right) {
    return left.content == right.content && left.stop_reason == right.stop_reason &&
           left.input_tokens == right.input_tokens &&
           left.output_tokens == right.output_tokens &&
           left.provider_request_id == right.provider_request_id;
}

}  // namespace agent

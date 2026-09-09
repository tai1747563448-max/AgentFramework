#pragma once

#include "adapters/json/value_json.h"
#include "domain/session_state.h"

#include <nlohmann/json.hpp>
#include <cmath>
#include <set>
#include <string_view>

// Shared helpers for tool-free model services.
namespace agent::model_memory_detail {

inline bool valid_text(std::string_view text, bool require_nonblank = false) {
    bool nonblank = false;
    for (std::size_t offset = 0; offset < text.size();) {
        const auto first = static_cast<unsigned char>(text[offset]);
        std::size_t width = 0;
        std::uint32_t point = 0;
        if (first > 0 && first <= 0x7f) { width = 1; point = first; }
        else if (first >= 0xc2 && first <= 0xdf) { width = 2; point = first & 0x1f; }
        else if (first >= 0xe0 && first <= 0xef) { width = 3; point = first & 0x0f; }
        else if (first >= 0xf0 && first <= 0xf4) { width = 4; point = first & 0x07; }
        else return false;
        if (offset + width > text.size()) return false;
        for (std::size_t index = 1; index < width; ++index) {
            const auto byte = static_cast<unsigned char>(text[offset + index]);
            if ((byte & 0xc0) != 0x80) return false;
            point = (point << 6) | (byte & 0x3f);
        }
        if ((width == 3 && point < 0x800) || (width == 4 && point < 0x10000) ||
            (point >= 0xd800 && point <= 0xdfff) || point > 0x10ffff) return false;
        const bool blank = point == ' ' || (point >= '\t' && point <= '\r') ||
            point == 0x85 || point == 0xa0 || point == 0x1680 ||
            (point >= 0x2000 && point <= 0x200a) || point == 0x2028 ||
            point == 0x2029 || point == 0x202f || point == 0x205f || point == 0x3000;
        nonblank = nonblank || !blank;
        offset += width;
    }
    return !require_nonblank || nonblank;
}

inline bool valid_value(const Value& value) {
    if (const auto* text = std::get_if<std::string>(&value.storage())) return valid_text(*text);
    if (value.is_double()) return std::isfinite(value.as_double());
    if (value.is_array()) {
        for (const auto& item : value.as_array()) if (!valid_value(item)) return false;
    }
    if (value.is_object()) {
        for (const auto& item : value.as_object()) {
            if (!valid_text(item.first) || !valid_value(item.second)) return false;
        }
    }
    return true;
}

inline bool valid_turns(const std::vector<CommittedSessionTurn>& turns) {
    if (turns.empty()) return false;
    std::uint64_t previous = 0;
    std::set<std::string> task_ids;
    for (const auto& turn : turns) {
        // Failed turns leave index gaps; require unique, increasing indexes.
        if (turn.turn_index <= previous ||
            !valid_text(turn.task_id, true) || !task_ids.insert(turn.task_id).second ||
            turn.messages.empty() || !conversation_history_is_valid(turn.messages)) return false;
        for (const auto& message : turn.messages) {
            for (const auto& block : message.content) {
                if (const auto* text = std::get_if<TextBlock>(&block)) {
                    if (!valid_text(text->text, true)) return false;
                } else if (const auto* use = std::get_if<ToolUseBlock>(&block)) {
                    if (!valid_text(use->call.id, true) || !valid_text(use->call.name, true) ||
                        !valid_value(use->call.arguments)) return false;
                } else {
                    const auto& result = std::get<ToolResultBlock>(block).result;
                    if (!valid_text(result.tool_call_id, true) || !valid_text(result.content)) return false;
                }
            }
        }
        previous = turn.turn_index;
    }
    return true;
}

inline nlohmann::json transcript(const std::vector<CommittedSessionTurn>& turns) {
    auto serialized = nlohmann::json::array();
    for (const auto& turn : turns) {
        auto messages = nlohmann::json::array();
        for (const auto& message : turn.messages) {
            auto content = nlohmann::json::array();
            for (const auto& block : message.content) {
                if (const auto* text = std::get_if<TextBlock>(&block)) {
                    content.push_back({{"type", "text"}, {"text", text->text}});
                } else if (const auto* use = std::get_if<ToolUseBlock>(&block)) {
                    content.push_back({{"type", "tool_use"}, {"id", use->call.id},
                        {"name", use->call.name}, {"arguments", value_to_json(use->call.arguments)}});
                } else {
                    const auto& result = std::get<ToolResultBlock>(block).result;
                    content.push_back({{"type", "tool_result"}, {"tool_call_id", result.tool_call_id},
                        {"content", result.content}, {"is_error", result.is_error}});
                }
            }
            messages.push_back({{"role", message.role == Role::User ? "user" : "assistant"},
                                {"content", std::move(content)}});
        }
        serialized.push_back({{"turn_index", turn.turn_index}, {"task_id", turn.task_id},
                              {"messages", std::move(messages)}});
    }
    return serialized;
}

inline const std::string* terminal_text(const ModelResponse& response) {
    if ((response.stop_reason != StopReason::EndTurn &&
         response.stop_reason != StopReason::StopSequence) ||
        !is_known_stop_reason_pair(response.stop_reason, response.raw_stop_reason) ||
        response.content.size() != 1) return nullptr;
    const auto* text = std::get_if<TextBlock>(&response.content.front());
    return text != nullptr && valid_text(text->text, true) ? &text->text : nullptr;
}

} // namespace agent::model_memory_detail

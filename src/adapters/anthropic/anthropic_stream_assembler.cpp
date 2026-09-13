#include "adapters/anthropic/anthropic_stream_assembler.h"
#include "adapters/anthropic/bounded_stream_json.h"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace agent {
namespace {
constexpr std::size_t kMaxContentBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaxToolBytes = 1024 * 1024;
constexpr std::size_t kMaxBlocks = 4096;

void require(bool condition) {
    if (!condition) throw std::invalid_argument("invalid stream lifecycle");
}

std::size_t count(const nlohmann::json& value) {
    require(value.is_number_integer());
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        require(number <= (std::numeric_limits<std::size_t>::max)());
        return static_cast<std::size_t>(number);
    }
    const auto number = value.get<std::int64_t>();
    require(number >= 0);
    return static_cast<std::size_t>(number);
}
}

AnthropicStreamAssembler::AnthropicStreamAssembler(ModelStreamObserver observer)
    : observer_(std::move(observer)) {}

void AnthropicStreamAssembler::notify(ModelStreamEventKind kind, const std::string& text) {
    if (!observer_) return;
    try {
        observer_({kind, block_index_, text});
    } catch (...) {
        observer_ = {};  // Presentation failure must not alter model acceptance.
    }
}

Result<void> AnthropicStreamAssembler::consume(const SseEvent& event) {
    if (error_) return Result<void>::failure(*error_);
    try {
        require(event.data.size() <= kMaxContentBytes - content_bytes_);
        content_bytes_ += event.data.size();
        const auto data = parse_bounded_stream_json(event.data);
        require(data.is_object());
        const auto type = data.at("type").get<std::string>();
        require(event.name.empty() || event.name == "message" || event.name == type);
        if (type == "error") {
            error_ = RuntimeError{ErrorCode::ProtocolFailure, "provider reported a stream error", false};
            return Result<void>::failure(*error_);
        }
        // Unknown auxiliary events are forward-compatible. Content deltas/blocks
        // are separately validated below and never silently discarded.
        if (type != "message_start" && type != "content_block_start" &&
            type != "content_block_delta" && type != "content_block_stop" &&
            type != "message_delta" && type != "message_stop") {
            return Result<void>::success();
        }
        require(!stopped_);
        if (type == "message_start") {
            require(!started_);
            message_ = data.at("message");
            require(message_.is_object());
            require(message_.at("type") == "message");
            require(message_.at("role") == "assistant");
            require(!message_.at("id").get<std::string>().empty());
            require(message_.at("content").is_array() && message_.at("content").empty());
            require(message_.at("stop_reason").is_null());
            count(message_.at("usage").at("input_tokens"));
            output_tokens_ = count(message_.at("usage").at("output_tokens"));
            started_ = true;
        } else {
            require(started_);
            if (type == "content_block_start") {
                require(!block_open_ && !message_delta_started_);
                require(count(data.at("index")) == block_index_ && block_index_ < kMaxBlocks);
                block_ = data.at("content_block");
                const auto block_type = block_.at("type").get<std::string>();
                require(block_type == "text" || block_type == "tool_use");
                if (block_type == "text") {
                    const auto initial_text = block_.at("text").get<std::string>();
                    if (!initial_text.empty()) notify(ModelStreamEventKind::TextDelta, initial_text);
                } else {
                    require(!block_.at("id").get<std::string>().empty());
                    require(!block_.at("name").get<std::string>().empty());
                    require(block_.at("input").is_object() && block_.at("input").empty());
                    tool_json_.clear();
                }
                block_open_ = true;
            } else if (type == "content_block_delta") {
                require(block_open_ && !final_delta_ && count(data.at("index")) == block_index_);
                const auto& delta = data.at("delta");
                const auto delta_type = delta.at("type").get<std::string>();
                if (block_.at("type") == "text") {
                    require(delta_type == "text_delta");
                    const auto text = delta.at("text").get<std::string>();
                    block_["text"].get_ref<std::string&>() += text;
                    if (!text.empty()) notify(ModelStreamEventKind::TextDelta, text);
                } else {
                    require(delta_type == "input_json_delta");
                    const auto fragment = delta.at("partial_json").get<std::string>();
                    require(fragment.size() <= kMaxToolBytes - tool_json_.size());
                    tool_json_ += fragment;
                }
            } else if (type == "content_block_stop") {
                require(block_open_ && !final_delta_ && count(data.at("index")) == block_index_);
                if (block_.at("type") == "tool_use" && !tool_json_.empty()) {
                    block_["input"] = parse_bounded_stream_json(tool_json_);
                    require(block_.at("input").is_object());
                }
                if (block_.at("type") == "text") notify(ModelStreamEventKind::TextBlockEnd);
                message_["content"].push_back(std::move(block_));
                tool_json_.clear();
                block_open_ = false;
                ++block_index_;
            } else if (type == "message_delta") {
                require(!block_open_ && !final_delta_);
                message_delta_started_ = true;
                const auto& delta = data.at("delta");
                const auto& usage = data.at("usage");
                const auto output = count(usage.at("output_tokens"));
                require(output >= output_tokens_);
                output_tokens_ = output;
                message_["usage"]["output_tokens"] = output;
                if (usage.contains("input_tokens")) {
                    // Compatible providers may defer input accounting until
                    // this delta. Usage is cumulative, so retain the latest
                    // count rather than adding it to the start-message value.
                    const auto input = count(usage.at("input_tokens"));
                    require(input >= count(message_.at("usage").at("input_tokens")));
                    message_["usage"]["input_tokens"] = input;
                }
                if (!delta.at("stop_reason").is_null()) {
                    const auto stop = delta.at("stop_reason").get<std::string>();
                    require(stop == "end_turn" || stop == "stop_sequence" ||
                            stop == "tool_use" || stop == "max_tokens");
                    message_["stop_reason"] = stop;
                    final_delta_ = true;
                }
            } else if (type == "message_stop") {
                require(!block_open_ && final_delta_);
                stopped_ = true;
            }
        }
        return Result<void>::success();
    } catch (...) {
        error_ = RuntimeError{ErrorCode::ProtocolFailure, "provider stream is invalid", false};
        return Result<void>::failure(*error_);
    }
}

Result<std::string> AnthropicStreamAssembler::finish() const {
    if (error_) return Result<std::string>::failure(*error_);
    if (!stopped_) {
        return Result<std::string>::failure(
            {ErrorCode::ProtocolFailure, "provider stream ended before message_stop", false});
    }
    try {
        return Result<std::string>::success(message_.dump());
    } catch (...) {
        return Result<std::string>::failure(
            {ErrorCode::ProtocolFailure, "provider stream is invalid", false});
    }
}

}  // namespace agent

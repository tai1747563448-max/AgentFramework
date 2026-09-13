#include "adapters/anthropic/sse_decoder.h"

#include <utility>

namespace agent {
namespace {
constexpr std::size_t kMaxLineBytes = 256 * 1024;
constexpr std::size_t kMaxEventBytes = 1024 * 1024;
constexpr std::size_t kMaxWireBytes = 32 * 1024 * 1024;
}

SseDecoder::SseDecoder(Observer observer) : observer_(std::move(observer)) {}

Result<void> SseDecoder::fail(const char* message) {
    error_ = RuntimeError{ErrorCode::ProtocolFailure, message, false};
    return Result<void>::failure(*error_);
}

Result<void> SseDecoder::consume_line() {
    if (line_.empty()) {
        if (has_data_) {
            event_.data.pop_back();  // SSE joins data fields using a newline.
            auto result = observer_(event_);
            if (!result.has_value()) {
                error_ = result.error();
                return result;
            }
        }
        event_ = {};
        has_data_ = false;
        return Result<void>::success();
    }
    if (line_.front() != ':') {
        const auto colon = line_.find(':');
        const auto field = line_.substr(0, colon);
        std::string_view value = colon == std::string::npos
                                     ? std::string_view{}
                                     : std::string_view(line_).substr(colon + 1);
        if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
        if (field == "data") {
            if (value.size() + 1 > kMaxEventBytes - event_.data.size()) {
                return fail("provider SSE event exceeds size limit");
            }
            event_.data.append(value.data(), value.size());
            event_.data.push_back('\n');
            has_data_ = true;
        } else if (field == "event") {
            event_.name.assign(value.data(), value.size());
        }
    }
    line_.clear();
    return Result<void>::success();
}

Result<void> SseDecoder::feed(std::string_view bytes) {
    if (error_) return Result<void>::failure(*error_);
    if (bytes.size() > kMaxWireBytes - wire_bytes_) {
        return fail("provider stream exceeds size limit");
    }
    wire_bytes_ += bytes.size();
    try {
        for (const char byte : bytes) {
            if (after_cr_) {
                after_cr_ = false;
                if (byte == '\n') continue;
            }
            if (byte == '\r' || byte == '\n') {
                auto result = consume_line();
                if (!result.has_value()) return result;
                after_cr_ = byte == '\r';
            } else {
                if (line_.size() == kMaxLineBytes) {
                    return fail("provider SSE line exceeds size limit");
                }
                line_.push_back(byte);
            }
        }
        return Result<void>::success();
    } catch (...) {
        return fail("provider SSE event is invalid");
    }
}

Result<void> SseDecoder::finish() {
    if (error_) return Result<void>::failure(*error_);
    if (!line_.empty() || has_data_ || !event_.name.empty()) {
        return fail("provider SSE stream ended inside an event");
    }
    return Result<void>::success();
}

}  // namespace agent

#pragma once

#include "adapters/anthropic/sse_decoder.h"
#include "domain/model_stream_event.h"

#include <nlohmann/json.hpp>

namespace agent {

// Assembles complete protocol blocks only. Tool arguments are never published.
class AnthropicStreamAssembler {
public:
    explicit AnthropicStreamAssembler(ModelStreamObserver observer);
    Result<void> consume(const SseEvent& event);
    Result<std::string> finish() const;

private:
    void notify(ModelStreamEventKind kind, const std::string& text = {});
    nlohmann::json message_;
    nlohmann::json block_;
    ModelStreamObserver observer_;
    std::string tool_json_;
    std::size_t content_bytes_{0};
    std::size_t block_index_{0};
    std::size_t output_tokens_{0};
    bool started_{false};
    bool block_open_{false};
    bool message_delta_started_{false};
    bool final_delta_{false};
    bool stopped_{false};
    std::optional<RuntimeError> error_;
};

}  // namespace agent

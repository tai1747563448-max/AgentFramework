#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace agent {

class Cancellation;

enum class ModelStreamEventKind { TextDelta, TextBlockEnd };

struct ModelStreamEvent {
    ModelStreamEventKind kind{ModelStreamEventKind::TextDelta};
    std::size_t block_index{0};
    std::string text;
};

using ModelStreamObserver = std::function<void(const ModelStreamEvent&)>;

struct ModelCallOptions {
    bool stream{false};
    const Cancellation* cancellation{nullptr};
    ModelStreamObserver observer;
    // Optional latency trace request id. Carried by AnthropicMessagesClient
    // into the HttpRequest so cpr_http_transport can attribute the
    // first_text_received sample to the right turn without coupling to the
    // runtime engine.
    std::string latency_request_id;
};

}  // namespace agent

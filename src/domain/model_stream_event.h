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
};

}  // namespace agent

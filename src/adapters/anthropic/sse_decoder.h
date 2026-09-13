#pragma once

#include "domain/result.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace agent {

struct SseEvent {
    std::string name;
    std::string data;
};

// Owns incomplete lines across network writes; no callback sees partial JSON.
class SseDecoder {
public:
    using Observer = std::function<Result<void>(const SseEvent&)>;
    explicit SseDecoder(Observer observer);
    Result<void> feed(std::string_view bytes);
    Result<void> finish();

private:
    Result<void> consume_line();
    Result<void> fail(const char* message);
    Observer observer_;
    std::string line_;
    SseEvent event_;
    std::size_t wire_bytes_{0};
    bool after_cr_{false};
    bool has_data_{false};
    std::optional<RuntimeError> error_;
};

}  // namespace agent

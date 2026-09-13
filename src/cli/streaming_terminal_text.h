#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace agent {

// Incremental version of render_terminal_text. The limit belongs to a whole
// model message, including all its text blocks, rather than an individual delta.
class StreamingTerminalText {
public:
    std::string append(std::string_view bytes);
    std::string finish();

private:
    std::string consume(std::string_view bytes, bool final);
    std::string pending_;
    std::size_t rendered_bytes_{0};
    bool truncated_{false};
};

}  // namespace agent

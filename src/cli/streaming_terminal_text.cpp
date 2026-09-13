#include "cli/streaming_terminal_text.h"
#include "cli/terminal_text.h"

#include <algorithm>

namespace agent {
namespace {

std::size_t unit_length(unsigned char first) {
    if (first >= 0xC2 && first <= 0xDF) return 2;
    if (first >= 0xE0 && first <= 0xEF) return 3;
    if (first >= 0xF0 && first <= 0xF4) return 4;
    return 1;
}

bool possible_prefix(std::string_view text, std::size_t count) {
    const auto first = static_cast<unsigned char>(text.front());
    for (std::size_t index = 1; index < count; ++index) {
        const auto byte = static_cast<unsigned char>(text[index]);
        if (byte < 0x80 || byte > 0xBF) return false;
        if (index == 1 && ((first == 0xE0 && byte < 0xA0) ||
            (first == 0xED && byte > 0x9F) ||
            (first == 0xF0 && byte < 0x90) ||
            (first == 0xF4 && byte > 0x8F))) return false;
    }
    return true;
}

}  // namespace

std::string StreamingTerminalText::append(std::string_view bytes) {
    return consume(bytes, false);
}

std::string StreamingTerminalText::finish() {
    return consume({}, true);
}

std::string StreamingTerminalText::consume(std::string_view bytes, bool final) {
    if (truncated_) return {};
    std::string joined = std::move(pending_);
    pending_.clear();
    if (!bytes.empty()) joined.append(bytes.data(), bytes.size());
    std::string rendered;
    rendered.reserve(std::min<std::size_t>(joined.size(), 8192));
    for (std::size_t offset = 0; offset < joined.size();) {
        const std::string_view remaining(joined.data() + offset, joined.size() - offset);
        auto length = unit_length(static_cast<unsigned char>(remaining.front()));
        if (remaining.size() < length && possible_prefix(remaining, remaining.size())) {
            if (!final) {
                pending_.assign(remaining);
                break;
            }
            length = 1;
        } else if (!possible_prefix(remaining, std::min(length, remaining.size()))) {
            length = 1;
        }
        const auto token = render_terminal_text(remaining.substr(0, length));
        if (rendered_bytes_ + token.size() > 8192) {
            rendered += "\n[output truncated]";
            truncated_ = true;
            pending_.clear();
            break;
        }
        rendered += token;
        rendered_bytes_ += token.size();
        offset += length;
    }
    return rendered;
}

}  // namespace agent

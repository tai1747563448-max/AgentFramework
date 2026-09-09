#include "cli/terminal_text.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace agent {
namespace {

char hex_digit(unsigned int value) {
    return "0123456789ABCDEF"[value & 0x0FU];
}

std::string byte_escape(unsigned char byte) {
    std::string escaped{"\\x00"};
    escaped[2] = hex_digit(byte >> 4U);
    escaped[3] = hex_digit(byte);
    return escaped;
}

std::string control_escape(std::uint32_t code_point) {
    std::string escaped{"\\u0000"};
    escaped[4] = hex_digit(code_point >> 4U);
    escaped[5] = hex_digit(code_point);
    return escaped;
}

bool continuation(unsigned char byte) {
    return byte >= 0x80U && byte <= 0xBFU;
}

struct Utf8Unit {
    bool valid{false};
    std::size_t length{1};
    std::uint32_t code_point{0};
};

Utf8Unit decode_utf8_unit(std::string_view text, std::size_t offset) {
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first <= 0x7FU) {
        return {true, 1, first};
    }
    if (first >= 0xC2U && first <= 0xDFU && offset + 1 < text.size()) {
        const auto second = static_cast<unsigned char>(text[offset + 1]);
        if (continuation(second)) {
            return {true, 2,
                    static_cast<std::uint32_t>(((first & 0x1FU) << 6U) |
                                               (second & 0x3FU))};
        }
    }
    if (first >= 0xE0U && first <= 0xEFU && offset + 2 < text.size()) {
        const auto second = static_cast<unsigned char>(text[offset + 1]);
        const auto third = static_cast<unsigned char>(text[offset + 2]);
        const bool valid_second =
            continuation(second) &&
            (first != 0xE0U || second >= 0xA0U) &&
            (first != 0xEDU || second <= 0x9FU);
        if (valid_second && continuation(third)) {
            return {true, 3,
                    static_cast<std::uint32_t>(((first & 0x0FU) << 12U) |
                                               ((second & 0x3FU) << 6U) |
                                               (third & 0x3FU))};
        }
    }
    if (first >= 0xF0U && first <= 0xF4U && offset + 3 < text.size()) {
        const auto second = static_cast<unsigned char>(text[offset + 1]);
        const auto third = static_cast<unsigned char>(text[offset + 2]);
        const auto fourth = static_cast<unsigned char>(text[offset + 3]);
        const bool valid_second =
            continuation(second) &&
            (first != 0xF0U || second >= 0x90U) &&
            (first != 0xF4U || second <= 0x8FU);
        if (valid_second && continuation(third) && continuation(fourth)) {
            return {true, 4,
                    static_cast<std::uint32_t>(((first & 0x07U) << 18U) |
                                               ((second & 0x3FU) << 12U) |
                                               ((third & 0x3FU) << 6U) |
                                               (fourth & 0x3FU))};
        }
    }
    return {};
}

}  // namespace

bool configure_interactive_terminal_utf8() noexcept {
#if defined(_WIN32)
    DWORD mode = 0;
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (input != nullptr && input != INVALID_HANDLE_VALUE &&
        GetConsoleMode(input, &mode) != 0 && SetConsoleCP(CP_UTF8) == 0) {
        return false;
    }
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output != nullptr && output != INVALID_HANDLE_VALUE &&
        GetConsoleMode(output, &mode) != 0 &&
        SetConsoleOutputCP(CP_UTF8) == 0) {
        return false;
    }
#endif
    return true;
}

std::string render_terminal_text(std::string_view text) {
    constexpr std::size_t kRenderedLimit = 8192;
    constexpr std::string_view kTruncationMarker = "\n[output truncated]";
    std::string rendered;
    rendered.reserve(text.size() < kRenderedLimit ? text.size()
                                                   : kRenderedLimit);
    bool truncated = false;
    for (std::size_t offset = 0; offset < text.size();) {
        const auto unit = decode_utf8_unit(text, offset);
        std::string escaped;
        std::string_view token;
        if (!unit.valid) {
            escaped = byte_escape(static_cast<unsigned char>(text[offset]));
            token = escaped;
        } else if ((unit.code_point < 0x20U && unit.code_point != '\n' &&
                    unit.code_point != '\t') ||
                   unit.code_point == 0x7FU ||
                   (unit.code_point >= 0x80U && unit.code_point <= 0x9FU)) {
            escaped = control_escape(unit.code_point);
            token = escaped;
        } else {
            token = text.substr(offset, unit.length);
        }
        if (rendered.size() + token.size() > kRenderedLimit) {
            truncated = true;
            break;
        }
        rendered.append(token.data(), token.size());
        offset += unit.valid ? unit.length : 1;
    }
    if (truncated) {
        rendered.append(kTruncationMarker.data(), kTruncationMarker.size());
    }
    return rendered;
}

}  // namespace agent

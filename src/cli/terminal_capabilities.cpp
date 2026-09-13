#include "cli/terminal_capabilities.h"
#include "cli/terminal_text.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#include <sys/ioctl.h>
#endif

namespace agent {

struct TerminalCapabilities::State {
    bool dynamic{false};
    bool utf8_ready{true};
#if defined(_WIN32)
    HANDLE output{INVALID_HANDLE_VALUE};
    DWORD original_mode{0};
    bool restore_mode{false};
    UINT input_cp{0};
    UINT output_cp{0};
#endif
};

TerminalCapabilities::TerminalCapabilities(std::istream& input,
                                           std::ostream& output, bool plain)
    : state_(std::make_unique<State>()) {
    if (&input != &std::cin || &output != &std::cout) return;
    const auto* term = std::getenv("TERM");
    const bool dumb = term != nullptr && std::string_view(term) == "dumb";
#if defined(_WIN32)
    DWORD input_mode = 0;
    const auto in = GetStdHandle(STD_INPUT_HANDLE);
    const bool console_input = in != nullptr && in != INVALID_HANDLE_VALUE &&
        GetConsoleMode(in, &input_mode) != 0;
    state_->output = GetStdHandle(STD_OUTPUT_HANDLE);
    const bool console_output = state_->output != nullptr &&
        state_->output != INVALID_HANDLE_VALUE &&
        GetConsoleMode(state_->output, &state_->original_mode) != 0;
    if (console_input) state_->input_cp = GetConsoleCP();
    if (console_output) state_->output_cp = GetConsoleOutputCP();
    state_->utf8_ready = configure_interactive_terminal_utf8();
    if (!plain && !dumb && console_input && console_output &&
        SetConsoleMode(state_->output, state_->original_mode |
                       ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) {
        state_->restore_mode = true;
        state_->dynamic = true;
    }
#else
    state_->dynamic = !plain && !dumb && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
#endif
}

TerminalCapabilities::~TerminalCapabilities() {
#if defined(_WIN32)
    if (state_->restore_mode) SetConsoleMode(state_->output, state_->original_mode);
    if (state_->input_cp != 0) SetConsoleCP(state_->input_cp);
    if (state_->output_cp != 0) SetConsoleOutputCP(state_->output_cp);
#endif
}

bool TerminalCapabilities::dynamic() const noexcept { return state_->dynamic; }
bool TerminalCapabilities::utf8_ready() const noexcept { return state_->utf8_ready; }

std::size_t TerminalCapabilities::columns() const noexcept {
#if defined(_WIN32)
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (state_->dynamic && GetConsoleScreenBufferInfo(state_->output, &info)) {
        return static_cast<std::size_t>(info.srWindow.Right - info.srWindow.Left + 1);
    }
#else
    winsize size{};
    if (state_->dynamic && ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
        return size.ws_col;
    }
#endif
    return 80;
}

}  // namespace agent

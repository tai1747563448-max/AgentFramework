#include "cli/picker.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <iostream>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <io.h>
#include <windows.h>
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace agent {
namespace {

// T23 palette (kept in sync with terminal_presenter.cpp): dim grey
// for the title, bold for the selected row, plain for the rest.
constexpr const char* kReset = "\x1B[0m";
constexpr const char* kBold = "\x1B[1m";
constexpr const char* kDim = "\x1B[2m";
constexpr const char* kInverse = "\x1B[7m";

std::size_t column_budget(const std::function<std::size_t()>& columns) {
    if (!columns) return 80;
    const auto value = columns();
    return value > 4 ? value - 2 : 80;
}

std::string truncate(const std::string& text, std::size_t width) {
    if (width == 0) return text;
    if (text.size() <= width) return text;
    if (width <= 3) return text.substr(0, width);
    return text.substr(0, width - 3) + "...";
}

class RawMode {
public:
    RawMode() {
#if defined(_WIN32)
        // _isatty rejects redirected stdin so CI / pipe-driven harnesses
        // automatically fall through to the std::getline path below.
        if (!_isatty(_fileno(stdin))) {
            enabled_ = false;
            return;
        }
        handle_ = GetStdHandle(STD_INPUT_HANDLE);
        if (handle_ == INVALID_HANDLE_VALUE) {
            enabled_ = false;
            return;
        }
        if (!GetConsoleMode(handle_, &previous_)) {
            enabled_ = false;
            return;
        }
        DWORD requested = previous_;
        requested &= ~static_cast<DWORD>(ENABLE_LINE_INPUT);
        requested &= ~static_cast<DWORD>(ENABLE_ECHO_INPUT);
        if (!SetConsoleMode(handle_, requested)) {
            enabled_ = false;
            return;
        }
        enabled_ = true;
#else
        if (!isatty(STDIN_FILENO)) {
            enabled_ = false;
            return;
        }
        if (tcgetattr(STDIN_FILENO, &previous_) != 0) {
            enabled_ = false;
            return;
        }
        termios raw = previous_;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 1;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
            enabled_ = false;
            return;
        }
        enabled_ = true;
#endif
    }
    ~RawMode() {
        if (!enabled_) return;
#if defined(_WIN32)
        SetConsoleMode(handle_, previous_);
#else
        tcsetattr(STDIN_FILENO, TCSANOW, &previous_);
#endif
    }
    RawMode(const RawMode&) = delete;
    RawMode& operator=(const RawMode&) = delete;

    bool enabled() const noexcept { return enabled_; }

    // Try to read one byte with a millisecond timeout. On Windows we
    // fall back to _kbhit + _getch_nolock; on POSIX the termios VMIN/
    // VTIME settings above yield a non-blocking read.
    bool read(char& byte) {
#if defined(_WIN32)
        if (!_kbhit()) return false;
        const int value = _getch_nolock();
        if (value == EOF) return false;
        byte = static_cast<char>(value);
        return true;
#else
        unsigned char raw_byte;
        const ssize_t read = ::read(STDIN_FILENO, &raw_byte, 1);
        if (read != 1) return false;
        byte = static_cast<char>(raw_byte);
        return true;
#endif
    }

private:
    bool enabled_{false};
#if defined(_WIN32)
    HANDLE handle_{nullptr};
    DWORD previous_{0};
#else
    termios previous_{};
#endif
};

}  // namespace

ListPicker::ListPicker(std::istream& input,
                       std::ostream& output,
                       std::vector<PickerItem> items,
                       std::string title,
                       std::function<std::size_t()> columns)
    : input_(input), output_(output), items_(std::move(items)),
      title_(std::move(title)), columns_(std::move(columns)) {}

void ListPicker::render() {
    const auto budget = column_budget(columns_);
    output_ << "\r\x1B[2K";
    if (!title_.empty()) {
        output_ << kDim << truncate(title_, budget) << kReset << '\n';
    }
    for (std::size_t index = 0; index < items_.size(); ++index) {
        output_ << "\r\x1B[2K";
        const auto& item = items_[index];
        const bool selected = static_cast<int>(index) == cursor_;
        if (selected) output_ << kInverse;
        std::string row;
        for (std::size_t column = 0; column < item.columns.size(); ++column) {
            if (column > 0) row += "  ";
            row += item.columns[column].value;
        }
        output_ << (selected ? kBold : "") << truncate(row, budget)
                << (selected ? kReset : "") << '\n';
    }
}

void ListPicker::move(int delta) {
    if (items_.empty()) return;
    int next = cursor_ + delta;
    const int total = static_cast<int>(items_.size());
    next = ((next % total) + total) % total;
    cursor_ = next;
    render();
}

void ListPicker::on_enter() {
    active_ = false;
}

void ListPicker::on_cancel() {
    active_ = false;
}

bool ListPicker::read_byte(char& byte) {
    // Stream path is used when raw mode is not available (stdin not a
    // tty, redirected input, etc.). Callers block on getline and pass
    // the result back to the picker via input_.
    return bool(input_.get(byte));
}

PickerOutcome ListPicker::run() {
    if (items_.empty()) {
        output_ << "(no items)\n";
        return {PickerOutcome::Kind::Cancelled, {}, -1};
    }
    cursor_ = 0;
    active_ = true;
    RawMode raw;
    output_ << "\x1B[?25l";  // hide cursor
    render();
    bool cancelled = false;

    // Raw mode is unavailable when stdin is not a tty (CI, piped
    // harnesses, the unit tests). In that mode we collect one full
    // line via std::getline and treat it as a numeric index, so the
    // same widget works under the interactive REPL and the headless
    // build pipeline.
    if (!raw.enabled()) {
        output_ << "select [0.." << (items_.size() - 1) << "]: ";
        output_.flush();
        std::string line;
        if (!std::getline(input_, line)) {
            cancelled = true;
        } else {
            int parsed = -1;
            try { parsed = std::stoi(line); } catch (...) {}
            if (parsed < 0 || parsed >= static_cast<int>(items_.size())) {
                cancelled = true;
            } else {
                cursor_ = parsed;
            }
        }
        output_ << "\x1B[?25h";
        if (cancelled) {
            return {PickerOutcome::Kind::Cancelled, {}, -1};
        }
        PickerOutcome outcome;
        outcome.index = cursor_;
        outcome.id = items_[cursor_].id;
        outcome.kind = PickerOutcome::Kind::Selected;
        return outcome;
    }

    std::string esc_buffer;
    while (active_) {
        char byte = 0;
        if (!raw.read(byte)) {
            // No raw byte available yet; loop until one arrives.
            continue;
        }
        if (!esc_buffer.empty()) {
            esc_buffer.push_back(byte);
            if (esc_buffer == "\x1B[A") { esc_buffer.clear(); move(-1); continue; }
            if (esc_buffer == "\x1B[B") { esc_buffer.clear(); move(1); continue; }
            if (esc_buffer.size() >= 3) esc_buffer.clear();
            continue;
        }
        if (byte == '\x1B') {
            esc_buffer.push_back(byte);
            continue;
        }
        if (byte == '\r' || byte == '\n') {
            on_enter();
            break;
        }
        if (byte == 'q' || byte == 3) {
            cancelled = true;
            on_cancel();
            break;
        }
        if (byte == 'j') { move(1); continue; }
        if (byte == 'k') { move(-1); continue; }
    }
    output_ << "\x1B[?25h";  // show cursor
    if (cancelled) {
        return {PickerOutcome::Kind::Cancelled, {}, -1};
    }
    if (cursor_ < 0 || cursor_ >= static_cast<int>(items_.size())) {
        return {PickerOutcome::Kind::Cancelled, {}, -1};
    }
    PickerOutcome outcome;
    outcome.index = cursor_;
    outcome.id = items_[cursor_].id;
    outcome.kind = PickerOutcome::Kind::Selected;
    return outcome;
}

}  // namespace agent
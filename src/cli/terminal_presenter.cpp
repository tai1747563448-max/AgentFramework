#include "cli/terminal_presenter.h"
#include "cli/terminal_text.h"
#include "domain/latency_trace.h"

#include <algorithm>
#include <iomanip>
#include <ostream>
#include <sstream>

namespace agent {
namespace {

std::string single_line(const std::string& raw) {
    auto label = render_terminal_text(raw);
    for (auto& byte : label) {
        if (byte == '\n' || byte == '\t') byte = ' ';
    }
    return label;
}

std::string fit_line(const std::string& text, std::size_t width) {
    std::size_t offset = 0, used = 0;
    while (offset < text.size()) {
        const auto first = static_cast<unsigned char>(text[offset]);
        const std::size_t bytes = first < 0x80 ? 1 : first < 0xE0 ? 2 : first < 0xF0 ? 3 : 4;
        // Conservatively reserve two cells for all non-ASCII glyphs. Statuses
        // stay on one line even for CJK; the transcript uses natural wrapping.
        const std::size_t cells = bytes == 1 ? 1 : 2;
        if (used + cells > width || offset + bytes > text.size()) break;
        offset += bytes;
        used += cells;
    }
    return text.substr(0, offset);
}

}  // namespace

TerminalPresenter::TerminalPresenter(std::ostream& output, bool dynamic,
                                     std::function<std::size_t()> columns)
    : output_(output), dynamic_(dynamic), columns_(std::move(columns)) {}

TerminalPresenter::~TerminalPresenter() {
    try { clear_status(); close_partial_line(); restore_cursor(); } catch (...) {}
}

void TerminalPresenter::begin() {
    active_ = true;
    status_visible_ = partial_line_ = preview_ = any_preview_ = false;
    round_.reset();
    sanitizer_ = StreamingTerminalText{};
    rendered_.clear();
    phase_.clear();
    rendered_emitted_ = false;
    if (dynamic_) output_ << "\x1b[?25l";
}

void TerminalPresenter::clear_status() {
    if (!status_visible_) return;
    output_ << "\r\x1b[2K";
    status_visible_ = false;
}

void TerminalPresenter::close_partial_line() {
    if (partial_line_) output_ << '\n';
    partial_line_ = false;
}

void TerminalPresenter::restore_cursor() {
    if (active_ && dynamic_) output_ << "\x1b[?25h";
    active_ = false;
    output_.flush();
}

void TerminalPresenter::phase(const std::string& label) {
    auto safe = single_line(label);
    if (safe.empty() || phase_ == safe) return;
    clear_status();
    close_partial_line();
    phase_ = std::move(safe);
    if (!dynamic_) {
        output_ << phase_ << '\n';
        output_.flush();
    }
}

void TerminalPresenter::progress(const RuntimeProgress& progress) {
    switch (progress.event_kind) {
    case EventKind::ContextPreparationStarted: phase("Preparing context..."); break;
    case EventKind::ModelCallStarted: {
        // A response may produce no preview at all. Its final text must never
        // be reconciled against a preceding tool round's provisional prefix.
        const auto tail = sanitizer_.finish();
        clear_status();
        if (!tail.empty()) {
            if (!preview_) output_ << "[Generating; provisional]\n";
            any_preview_ = true;
            write_text(tail);
        }
        close_partial_line();
        round_.reset();
        sanitizer_ = StreamingTerminalText{};
        rendered_.clear();
        preview_ = false;
        phase("Thinking...");
        break;
    }
    case EventKind::ToolCallStarted:
        phase(progress.tool_name.empty() ? "Running tool..." : "Running tool: " + progress.tool_name);
        break;
    case EventKind::ToolCallSucceeded:
    case EventKind::ToolCallFailed: {
        clear_status();
        close_partial_line();
        phase_ = progress.event_kind == EventKind::ToolCallSucceeded
            ? "Tool completed" : "Tool failed";
        phase_ += progress.tool_name.empty() ? "." : ": " + single_line(progress.tool_name);
        const auto columns = columns_ ? columns_() : 80;
        output_ << (dynamic_ ? fit_line(phase_, columns > 1 ? columns - 1 : 0) : phase_) << '\n';
        output_.flush();
        break;
    }
    default: break;
    }
}

void TerminalPresenter::write_text(const std::string& text) {
    if (text.empty()) return;
    clear_status();
    output_ << text;
    partial_line_ = text.back() != '\n';
    output_.flush();
    // T0 latency trace hook. Emit first_text_rendered the first time we
    // successfully write actual model content to the terminal. Status
    // messages and tick animations do not count; only an actual text chunk
    // does. The round/task identifier is provided through the previous
    // text() callback via the rendered_emitted_ latch below.
    if (!rendered_emitted_) {
        rendered_emitted_ = true;
        emit_latency_sample(pending_request_id_, kStageFirstTextRendered);
    }
}

void TerminalPresenter::text(const RuntimeTextUpdate& update) {
    if (!round_ || *round_ != update.model_round) {
        if (round_) {
            write_text(sanitizer_.finish());
            close_partial_line();
        }
        round_ = update.model_round;
        sanitizer_ = StreamingTerminalText{};
        rendered_.clear();
        preview_ = false;
        // A new model round restarts the first-text rendering latch so the
        // next actual write emits a fresh sample for the new turn.
        rendered_emitted_ = false;
        pending_request_id_ = update.task_id;
    } else if (pending_request_id_.empty()) {
        pending_request_id_ = update.task_id;
    }
    const auto safe = update.event.kind == ModelStreamEventKind::TextBlockEnd
        ? sanitizer_.finish() : sanitizer_.append(update.event.text);
    if (safe.empty()) return;
    clear_status();
    if (!preview_) {
        output_ << "[Generating; provisional]\n";
        preview_ = any_preview_ = true;
    }
    rendered_ += safe;
    write_text(safe);
}

void TerminalPresenter::tick(std::chrono::milliseconds elapsed) {
    if (!active_ || !dynamic_ || partial_line_) return;
    static constexpr char frames[] = "|/-\\";
    std::ostringstream status;
    status << frames[(elapsed.count() / 100) % 4] << ' '
           << (phase_.empty() ? "Preparing context..." : phase_) << "  "
           << std::fixed << std::setprecision(1) << (elapsed.count() / 1000.0) << "s";
    const auto columns = columns_ ? columns_() : 80;
    output_ << "\r\x1b[2K" << fit_line(status.str(), columns > 1 ? columns - 1 : 0);
    status_visible_ = true;
    output_.flush();
}

void TerminalPresenter::finish(bool success, const std::string& final_text) {
    clear_status();
    if (success) {
        // The committed answer supplies any bytes that a disabled observer did
        // not deliver. Do not escape a pending UTF-8 prefix before reconciling it.
        const auto final_rendered = render_terminal_text(final_text);
        if (!preview_) {
            write_text(final_rendered);
        } else if (final_rendered.compare(0, rendered_.size(), rendered_) == 0) {
            write_text(final_rendered.substr(rendered_.size()));
        } else {
            close_partial_line();
            output_ << "[Final answer]\n";
            write_text(final_rendered);
        }
        close_partial_line();
        if (any_preview_ || dynamic_) output_ << "[Turn complete]\n";
    } else {
        const auto tail = sanitizer_.finish();
        if (!tail.empty()) {
            if (!preview_) output_ << "[Generating; provisional]\n";
            preview_ = any_preview_ = true;
            rendered_ += tail;
            write_text(tail);
        }
        close_partial_line();
        if (any_preview_) output_ << "[Incomplete; turn was not committed]\n";
    }
    restore_cursor();
}

}  // namespace agent

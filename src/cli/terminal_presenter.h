#pragma once

#include "application/runtime_engine.h"
#include "cli/streaming_terminal_text.h"
#include "cli/theme.h"

#include <chrono>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>

namespace agent {

class TerminalPresenter {
public:
    // T18: theme_id selects the spinner glyph + verb vocabulary. The
    // default is the Claude-Code-parity theme; forks can pass
    // kFramesThemeMinimal (no animation) or kFramesThemeAscii
    // (ASCII-only animation) without recompiling presenter logic.
    TerminalPresenter(std::ostream& output, bool dynamic,
                      std::function<std::size_t()> columns = {},
                      ThemeId theme_id = ThemeId::Claude);
    ~TerminalPresenter();
    void begin();
    void phase(const std::string& label);
    void progress(const RuntimeProgress& progress);
    void text(const RuntimeTextUpdate& update);
    void tick(std::chrono::milliseconds elapsed);
    void finish(bool success, const std::string& final_text);

private:
    void clear_status();
    void write_text(const std::string& text);
    void close_partial_line();
    void restore_cursor();

    std::ostream& output_;
    bool dynamic_;
    std::function<std::size_t()> columns_;
    // T18: theme is fixed at construction time. The presenter is a
    // single-threaded UI component, so swapping themes mid-stream would
    // split the in-flight progress row in half; the request owner should
    // construct a new TerminalPresenter if they want a different theme.
    ThemeId theme_id_;
    bool active_{false};
    bool status_visible_{false};
    bool partial_line_{false};
    bool preview_{false};
    bool any_preview_{false};
    std::optional<std::size_t> round_;
    StreamingTerminalText sanitizer_;
    std::string rendered_;
    std::string phase_;
    // T0 latency trace state. rendered_emitted_ latches the first time the
    // presenter writes a real text chunk; pending_request_id_ remembers the
    // task id from the most recent RuntimeTextUpdate so the sample can be
    // attributed to the right turn.
    bool rendered_emitted_{false};
    std::string pending_request_id_;
    // T08: running token/cost accumulator. input_ is the latest cumulative
    // input reported by the provider (max across responses); output_ is
    // the sum of every response's output tokens so far. usd_ mirrors
    // output_ in dollar terms. The presenter takes input by MAX not SUM
    // because Anthropic reports cumulative input per response.
    std::size_t input_tokens_{0};
    std::size_t output_tokens_{0};
    double usd_total_{0.0};
};

}  // namespace agent

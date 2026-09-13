#pragma once

#include "application/runtime_engine.h"
#include "cli/streaming_terminal_text.h"

#include <chrono>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>

namespace agent {

class TerminalPresenter {
public:
    TerminalPresenter(std::ostream& output, bool dynamic,
                      std::function<std::size_t()> columns = {});
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
    bool active_{false};
    bool status_visible_{false};
    bool partial_line_{false};
    bool preview_{false};
    bool any_preview_{false};
    std::optional<std::size_t> round_;
    StreamingTerminalText sanitizer_;
    std::string rendered_;
    std::string phase_;
};

}  // namespace agent

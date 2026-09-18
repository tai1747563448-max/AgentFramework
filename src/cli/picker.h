#pragma once

// T23: in-REPL list picker. Reuses the presenter's ANSI SGR palette
// instead of dragging in a TUI library. Three call sites converge on
// the same widget:
//   - session selector (replacing the /resume <id> getline path)
//   - permission confirm (when T11 wires Ask decisions)
//   - model selector (forks that expose multiple model SKUs)
//
// The picker reads one byte at a time from std::cin and translates
// the ANSI escape sequences for arrow keys into integer indices. On
// platforms without a usable raw mode (Windows / non-tty stdin),
// the picker transparently falls back to the std::getline path
// the legacy commands used.
#include <cstddef>
#include <functional>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

namespace agent {

struct PickerColumn {
    // header / value / width cells. width == 0 means "no truncation".
    std::string header;
    std::string value;
    std::size_t width{0};
    bool right_align{false};
};

struct PickerItem {
    std::string id;            // stable identifier (session_id, model name, ...)
    std::vector<PickerColumn> columns;
};

struct PickerOutcome {
    enum class Kind { Selected, Cancelled, InvalidInput };
    Kind kind{Kind::Cancelled};
    std::string id;
    int index{-1};
};

class ListPicker {
public:
    ListPicker(std::istream& input,
               std::ostream& output,
               std::vector<PickerItem> items,
               std::string title = {},
               std::function<std::size_t()> columns = {});

    // run() blocks until the user selects an item, cancels, or the
    // input stream closes. It always restores the cursor + line state
    // before returning, even when an exception is thrown.
    PickerOutcome run();

private:
    bool read_byte(char& byte);
    void render();
    void move(int delta);
    void on_enter();
    void on_cancel();

    std::istream& input_;
    std::ostream& output_;
    std::vector<PickerItem> items_;
    std::string title_;
    std::function<std::size_t()> columns_;
    int cursor_{0};
    bool active_{false};
};

}  // namespace agent
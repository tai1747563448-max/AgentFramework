#pragma once

#include <cstddef>
#include <iosfwd>
#include <memory>

namespace agent {

// Console settings are restored even when a turn or initialization throws.
class TerminalCapabilities {
public:
    TerminalCapabilities(std::istream& input, std::ostream& output, bool plain);
    ~TerminalCapabilities();
    TerminalCapabilities(const TerminalCapabilities&) = delete;
    TerminalCapabilities& operator=(const TerminalCapabilities&) = delete;
    bool dynamic() const noexcept;
    bool utf8_ready() const noexcept;
    std::size_t columns() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace agent

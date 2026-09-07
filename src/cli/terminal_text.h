#pragma once

#include <string>
#include <string_view>

namespace agent {

bool configure_interactive_terminal_utf8() noexcept;
std::string render_terminal_text(std::string_view text);

}  // namespace agent

#pragma once

#include "adapters/workspace/workspace_types.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace agent::workspace {

bool is_strict_utf8_text(std::string_view text) noexcept;
std::string sha256_hex(std::string_view bytes);
Outcome<LinePage> line_page(std::string_view text,
                            std::size_t start_line,
                            std::size_t max_lines,
                            std::size_t max_content_bytes);
std::vector<std::size_t> literal_matches(std::string_view line,
                                         std::string_view query,
                                         bool case_sensitive);

}  // namespace agent::workspace

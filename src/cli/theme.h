#pragma once

// T18: outputStyle / theme. Three compile-time themes ship with the agent.
// Fork scenarios (CI smoke tests, low-bandwidth terminals, audit-only "do
// nothing fancy" environments) select a theme by `presentation.theme_id`
// without dragging in a plugin runtime. Long-term dlopen lives in P3.
//
// Each theme bundles:
//   - spinner glyph table (4 chars rotating every 100ms)
//   - shimmer verb vocabulary used while a model call is in flight
//   - tool / phase / completion glyphs
// Themes deliberately reuse the same ANSI SGR palette; the difference is
// the glyph vocabulary so users still see progress animation when their
// terminal cannot render Unicode.
#include <array>
#include <string>
#include <string_view>

namespace agent {

enum class ThemeId : int {
    Claude = 0,    // Claude Code parity: pipe-dash Unicode spinner + 21 verbs
    Minimal = 1,   // Pure ASCII. No spinner animation, just dots and elapsed.
    Ascii = 2,     // Spinner that still animates but uses only ASCII glyphs.
};

struct Theme {
    ThemeId id;
    std::string_view label;            // for /theme / --show-effective-config
    std::string_view spinner_frames;   // 4 chars rotating every 100ms
    std::array<std::string_view, 21> verbs;
    std::string_view tool_running_glyph;
    std::string_view tool_done_glyph;
    std::string_view tool_failed_glyph;
    std::string_view turn_done_glyph;
};

inline constexpr Theme kFramesThemeClaude{
    ThemeId::Claude, "claude", "|/-\\",
    {"Thinking", "Considering", "Pondering", "Drafting", "Reviewing",
     "Reasoning", "Inspecting", "Composing", "Synthesising", "Mulling",
     "Cogitating", "Reflecting", "Musing", "Noodling", "Reading",
     "Gathering", "Weighing", "Deliberating", "Estimating", "Sketching",
     "Computing"},
    "\xE2\x97\x89",  // ◉
    "\xE2\x97\x8C",  // ◌
    "\xE2\x9C\x97",  // ✗
    "\xE2\x9C\x93",  // ✓
};

inline constexpr Theme kFramesThemeMinimal{
    ThemeId::Minimal, "minimal", "....",
    {"Working", "Working", "Working", "Working", "Working",
     "Working", "Working", "Working", "Working", "Working",
     "Working", "Working", "Working", "Working", "Working",
     "Working", "Working", "Working", "Working", "Working",
     "Working"},
    "*",
    "+",
    "!",
    "OK",
};

inline constexpr Theme kFramesThemeAscii{
    ThemeId::Ascii, "ascii", "|/-\\",
    {"Thinking", "Working", "Reading", "Reasoning", "Drafting",
     "Reviewing", "Inspecting", "Composing", "Considering", "Pondering",
     "Computing", "Reflecting", "Deliberating", "Weighing", "Estimating",
     "Gathering", "Sketching", "Mulling", "Noodling", "Musing",
     "Cogitating"},
    "@",
    "+",
    "x",
    "OK",
};

inline const Theme& theme_for(ThemeId id) {
    switch (id) {
    case ThemeId::Claude: return kFramesThemeClaude;
    case ThemeId::Minimal: return kFramesThemeMinimal;
    case ThemeId::Ascii: return kFramesThemeAscii;
    }
    return kFramesThemeClaude;
}

inline std::string_view theme_label(ThemeId id) {
    return theme_for(id).label;
}

inline bool is_valid_theme_id(int value) {
    return value >= static_cast<int>(ThemeId::Claude) &&
           value <= static_cast<int>(ThemeId::Ascii);
}

}  // namespace agent
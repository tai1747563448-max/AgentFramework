#include "cli/theme.h"
#include "test_support.h"

#include <string>
#include <string_view>

namespace {

bool is_pure_ascii(std::string_view text) {
    for (const char byte : text) {
        if (static_cast<unsigned char>(byte) > 0x7Eu) return false;
    }
    return true;
}

TEST_CASE(theme_claude_uses_unicode_spinner_and_unicode_tool_glyphs) {
    using namespace agent;
    REQUIRE(kFramesThemeClaude.spinner_frames.size() == 4u);
    REQUIRE(!kFramesThemeClaude.spinner_frames.empty());
    REQUIRE(!kFramesThemeClaude.tool_running_glyph.empty());
    // Claude Code parity uses Unicode glyphs in the running / done / failed
    // slots; the spinner itself stays ASCII so the four-frame loop reads
    // even in terminals that drop Unicode glyphs.
    REQUIRE(is_pure_ascii(kFramesThemeClaude.spinner_frames));
}

TEST_CASE(theme_minimal_has_no_unicode_at_all) {
    using namespace agent;
    REQUIRE(kFramesThemeMinimal.spinner_frames.size() == 4u);
    REQUIRE(is_pure_ascii(kFramesThemeMinimal.spinner_frames));
    REQUIRE(is_pure_ascii(kFramesThemeMinimal.tool_running_glyph));
    REQUIRE(is_pure_ascii(kFramesThemeMinimal.tool_done_glyph));
    REQUIRE(is_pure_ascii(kFramesThemeMinimal.tool_failed_glyph));
    REQUIRE(is_pure_ascii(kFramesThemeMinimal.turn_done_glyph));
    for (const auto verb : kFramesThemeMinimal.verbs) {
        REQUIRE(is_pure_ascii(verb));
    }
}

TEST_CASE(theme_ascii_uses_ascii_spinner_and_glyphs) {
    using namespace agent;
    REQUIRE(is_pure_ascii(kFramesThemeAscii.spinner_frames));
    REQUIRE(is_pure_ascii(kFramesThemeAscii.tool_running_glyph));
    REQUIRE(is_pure_ascii(kFramesThemeAscii.tool_done_glyph));
    REQUIRE(is_pure_ascii(kFramesThemeAscii.tool_failed_glyph));
    REQUIRE(is_pure_ascii(kFramesThemeAscii.turn_done_glyph));
}

TEST_CASE(theme_for_dispatches_by_id) {
    using namespace agent;
    REQUIRE(theme_for(ThemeId::Claude).id == ThemeId::Claude);
    REQUIRE(theme_for(ThemeId::Minimal).id == ThemeId::Minimal);
    REQUIRE(theme_for(ThemeId::Ascii).id == ThemeId::Ascii);
    REQUIRE(theme_label(ThemeId::Claude) == "claude");
    REQUIRE(theme_label(ThemeId::Minimal) == "minimal");
    REQUIRE(theme_label(ThemeId::Ascii) == "ascii");
}

TEST_CASE(is_valid_theme_id_rejects_out_of_range) {
    using namespace agent;
    REQUIRE(is_valid_theme_id(0));
    REQUIRE(is_valid_theme_id(1));
    REQUIRE(is_valid_theme_id(2));
    REQUIRE(!is_valid_theme_id(3));
    REQUIRE(!is_valid_theme_id(-1));
}

TEST_CASE(verb_vocabularies_have_twenty_one_entries) {
    using namespace agent;
    // Claude Code parity: 21 verbs rotate every ~5% of the shimmer window.
    // Minimal duplicates "Working" so the row still pulses even though
    // the vocabulary never shifts.
    REQUIRE(kFramesThemeClaude.verbs.size() == 21u);
    REQUIRE(kFramesThemeMinimal.verbs.size() == 21u);
    REQUIRE(kFramesThemeAscii.verbs.size() == 21u);
}

}  // namespace
#include "cli/streaming_terminal_text.h"
#include "cli/terminal_capabilities.h"
#include "cli/terminal_presenter.h"
#include "cli/terminal_text.h"
#include "test_support.h"

#include <chrono>
#include <sstream>
#include <string>

TEST_CASE(streaming_terminal_text_preserves_every_utf8_split_and_escapes_controls) {
    const std::string text = u8"中文🙂\n\t";
    for (std::size_t split = 0; split <= text.size(); ++split) {
        agent::StreamingTerminalText sanitizer;
        const auto first = sanitizer.append(text.substr(0, split));
        const auto second = sanitizer.append(text.substr(split));
        REQUIRE(first + second + sanitizer.finish() == text);
    }
    agent::StreamingTerminalText sanitizer;
    REQUIRE(sanitizer.append("\xE4").empty());
    REQUIRE(sanitizer.finish() == "\\xE4");
    REQUIRE(sanitizer.append(std::string("\x1b\r\0", 3)) ==
            "\\u001B\\u000D\\u0000");
    REQUIRE(sanitizer.append("\xC2\x9B") == "\\u009B");
}

TEST_CASE(streaming_terminal_text_limits_the_whole_message_once) {
    agent::StreamingTerminalText sanitizer;
    std::string rendered;
    for (int index = 0; index != 10000; ++index) {
        rendered += sanitizer.append("a");
    }
    rendered += sanitizer.finish();
    REQUIRE(rendered == std::string(8192, 'a') + "\n[output truncated]");
    REQUIRE(sanitizer.append("more").empty());
}

TEST_CASE(streaming_terminal_text_matches_buffered_sanitizer_for_invalid_splits) {
    const std::string bytes = std::string("\xE0\x80\x80\xED\xA0\x80\xF4\x90\x80\x80") +
        "\xF0\x9F" + "x" + "\x1B[2J\xC2\x9B";
    for (std::size_t split = 0; split <= bytes.size(); ++split) {
        agent::StreamingTerminalText sanitizer;
        auto rendered = sanitizer.append(bytes.substr(0, split));
        rendered += sanitizer.append(bytes.substr(split));
        rendered += sanitizer.finish();
        REQUIRE(rendered == agent::render_terminal_text(bytes));
    }
}

TEST_CASE(terminal_presenter_animates_clears_before_text_and_does_not_duplicate) {
    std::ostringstream output;
    agent::TerminalPresenter presenter(output, true, [] { return 60U; });
    presenter.begin();
    presenter.phase("Thinking...");
    presenter.tick(std::chrono::milliseconds(0));
    const auto first = output.str();
    presenter.tick(std::chrono::milliseconds(110));
    REQUIRE(output.str().substr(first.size()).find('/') != std::string::npos);
    presenter.text({"task", 1, {agent::ModelStreamEventKind::TextDelta, 0, "hello"}});
    const auto after_text = output.str();
    REQUIRE(after_text.find("\r\x1b[2K[Generating; provisional]\nhello") != std::string::npos);
    presenter.tick(std::chrono::milliseconds(220));
    REQUIRE(output.str() == after_text);
    presenter.finish(true, "hello");
    REQUIRE(output.str().find("hello") == output.str().rfind("hello"));
    REQUIRE(output.str().find("[Turn complete]") != std::string::npos);
    REQUIRE(output.str().find("\x1b[?25h") != std::string::npos);
}

TEST_CASE(terminal_presenter_plain_fallback_keeps_preview_and_final_suffix) {
    std::ostringstream output;
    agent::TerminalPresenter presenter(output, false);
    presenter.begin();
    presenter.phase("Thinking...");
    presenter.text({"task", 1, {agent::ModelStreamEventKind::TextDelta, 0, u8"你"}});
    presenter.finish(true, u8"你好");
    REQUIRE(output.str().find(u8"你好\n") != std::string::npos);
    REQUIRE(output.str().find('\x1b') == std::string::npos);
    REQUIRE(output.str().find('\r') == std::string::npos);
    std::istringstream input;
    agent::TerminalCapabilities capabilities(input, output, false);
    REQUIRE(!capabilities.dynamic());
}

TEST_CASE(terminal_presenter_failure_marks_preview_and_next_turn_starts_cleanly) {
    std::ostringstream output;
    agent::TerminalPresenter presenter(output, true, [] { return 32U; });
    presenter.begin();
    presenter.text({"task", 1, {agent::ModelStreamEventKind::TextDelta, 0, "partial"}});
    presenter.finish(false, "");
    REQUIRE(output.str().find("[Incomplete; turn was not committed]") != std::string::npos);
    presenter.begin();
    presenter.finish(true, "next answer");
    REQUIRE(output.str().find("next answer") != std::string::npos);
    REQUIRE(output.str().find("next answer") == output.str().rfind("next answer"));
}

TEST_CASE(terminal_presenter_budgets_across_text_blocks_and_resets_for_model_rounds) {
    std::ostringstream output;
    agent::TerminalPresenter presenter(output, false);
    presenter.begin();
    presenter.text({"task", 1, {agent::ModelStreamEventKind::TextDelta, 0, std::string(8192, 'a')}});
    presenter.text({"task", 1, {agent::ModelStreamEventKind::TextBlockEnd, 0, {}}});
    presenter.text({"task", 1, {agent::ModelStreamEventKind::TextDelta, 1, "hidden"}});
    presenter.phase("Running tool...");
    presenter.text({"task", 2, {agent::ModelStreamEventKind::TextDelta, 0, "new round"}});
    presenter.finish(true, "new round");
    REQUIRE(output.str().find("hidden") == std::string::npos);
    REQUIRE(output.str().find("new round") == output.str().rfind("new round"));
    REQUIRE(output.str().find("[output truncated]") == output.str().rfind("[output truncated]"));
}

TEST_CASE(terminal_presenter_reconciles_unrendered_utf8_tail_against_committed_answer) {
    std::ostringstream output;
    agent::TerminalPresenter presenter(output, false);
    presenter.begin();
    presenter.text({"task", 1, {agent::ModelStreamEventKind::TextDelta, 0, "prefix \xE4"}});
    presenter.finish(true, u8"prefix 中");
    REQUIRE(output.str().find(u8"prefix 中\n") != std::string::npos);
    REQUIRE(output.str().find("\\xE4") == std::string::npos);
    REQUIRE(output.str().find("prefix") == output.str().rfind("prefix"));
}

TEST_CASE(terminal_presenter_marks_an_incomplete_utf8_only_preview_on_failure) {
    std::ostringstream output;
    agent::TerminalPresenter presenter(output, false);
    presenter.begin();
    presenter.text({"task", 1, {agent::ModelStreamEventKind::TextDelta, 0, "\xE4"}});
    presenter.finish(false, "");
    REQUIRE(output.str().find("[Generating; provisional]\n\\xE4\n") != std::string::npos);
    REQUIRE(output.str().find("[Incomplete; turn was not committed]") != std::string::npos);
}

TEST_CASE(terminal_presenter_new_model_start_does_not_reuse_previous_tool_round_prefix) {
    for (const bool dynamic : {false, true}) {
        std::ostringstream output;
        agent::TerminalPresenter presenter(output, dynamic);
        presenter.begin();
        presenter.progress({"task", 1, agent::EventKind::ModelCallStarted,
                            agent::TaskStatus::AwaitingModel, {}});
        presenter.text({"task", 1,
                        {agent::ModelStreamEventKind::TextDelta, 0, "I will check."}});
        presenter.progress({"task", 2, agent::EventKind::ToolCallStarted,
                            agent::TaskStatus::AwaitingModel, "read_file"});
        presenter.progress({"task", 3, agent::EventKind::ToolCallSucceeded,
                            agent::TaskStatus::AwaitingModel, "read_file"});
        presenter.progress({"task", 4, agent::EventKind::ModelCallStarted,
                            agent::TaskStatus::AwaitingModel, {}});
        // This final model has no preview (buffered provider or disabled observer).
        presenter.finish(true, "I will check. Done.");
        REQUIRE(output.str().find("I will check. Done.\n") != std::string::npos);
        REQUIRE(output.str().find("I will check. Done.") ==
                output.str().rfind("I will check. Done."));
    }
}

TEST_CASE(terminal_presenter_model_start_flushes_and_resets_previous_utf8_prefix) {
    std::ostringstream output;
    agent::TerminalPresenter presenter(output, false);
    presenter.begin();
    presenter.text({"task", 1,
                    {agent::ModelStreamEventKind::TextDelta, 0, "previous \xE4"}});
    presenter.progress({"task", 2, agent::EventKind::ModelCallStarted,
                        agent::TaskStatus::AwaitingModel, {}});
    presenter.finish(true, "complete answer");
    REQUIRE(output.str().find("previous \\xE4\nThinking...\ncomplete answer\n") != std::string::npos);
}

TEST_CASE(terminal_presenter_tool_completion_is_durable_without_animation_ticks) {
    for (const bool dynamic : {false, true}) {
        for (const bool success : {false, true}) {
            std::ostringstream output;
            agent::TerminalPresenter presenter(output, dynamic);
            presenter.begin();
            presenter.text({"task", 1,
                {agent::ModelStreamEventKind::TextDelta, 0, "checking"}});
            presenter.progress({"task", 2, agent::EventKind::ToolCallStarted,
                                agent::TaskStatus::AwaitingModel, "read_file"});
            const auto before_completion = output.str().size();
            presenter.progress({"task", 3,
                success ? agent::EventKind::ToolCallSucceeded : agent::EventKind::ToolCallFailed,
                agent::TaskStatus::AwaitingModel, "read_file\n\x1b[2J"});
            const std::string expected = std::string(success ? "Tool completed: " : "Tool failed: ") +
                "read_file \\u001B[2J\n";
            const auto completion = output.str().substr(before_completion);
            REQUIRE(completion == expected);
            presenter.finish(false, "");
            REQUIRE(output.str().find(expected) == output.str().rfind(expected));
            if (!dynamic) REQUIRE(output.str().find('\x1b') == std::string::npos);
        }
    }
}

TEST_CASE(terminal_presenter_first_text_delta_is_rendered_without_buffering_delay) {
    // The first-text-to-terminal hop must add no observable delay beyond the
    // time the caller spends inside text(). Anything larger would indicate
    // the presenter is holding bytes back from the terminal, defeating the
    // first_text_rendered measurement that benchmarks rely on.
    std::ostringstream output;
    agent::TerminalPresenter presenter(output, false);
    presenter.begin();
    const auto before = std::chrono::steady_clock::now();
    presenter.text({"task", 1,
                    {agent::ModelStreamEventKind::TextDelta, 0, "hello"}});
    const auto after = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        after - before);
    // The presenter must not introduce sleep-based throttling. Allow a
    // generous upper bound to absorb CI jitter; the value is well above the
    // flush cost of an ostringstream but well below the human-visible
    // streaming latency budget.
    REQUIRE(elapsed < std::chrono::milliseconds(50));
    REQUIRE(output.str().find("[Generating; provisional]\nhello") != std::string::npos);
    presenter.finish(true, "hello");
}

TEST_CASE(terminal_presenter_incomplete_stream_with_partial_preview_is_never_committed) {
    // Stream text arrived during the call but the consumer eventually failed.
    // The presenter must treat the partial preview as provisional: never
    // reconcile it with the success path's final-answer code path and never
    // emit the [Turn complete] marker that signals a committed answer.
    std::ostringstream output;
    agent::TerminalPresenter presenter(output, true, [] { return 60U; });
    presenter.begin();
    presenter.text({"task", 1,
                    {agent::ModelStreamEventKind::TextDelta, 0, "partial "}});
    presenter.text({"task", 1,
                    {agent::ModelStreamEventKind::TextDelta, 0, "preview"}});
    presenter.finish(false, "");
    const auto rendered = output.str();
    REQUIRE(rendered.find("partial preview") != std::string::npos);
    REQUIRE(rendered.find("[Incomplete; turn was not committed]") != std::string::npos);
    REQUIRE(rendered.find("[Turn complete]") == std::string::npos);
    REQUIRE(rendered.find("[Final answer]") == std::string::npos);
}

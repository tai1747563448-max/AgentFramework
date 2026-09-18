#include "commands/registry.h"
#include "test_support.h"

#include <sstream>
#include <string>

namespace {

bool invoke(const agent::CommandRegistry& registry,
            const std::string& line,
            std::string* captured = nullptr) {
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    agent::CommandContext ctx;
    ctx.input = &input;
    ctx.output = &output;
    ctx.error = &error;
    ctx.user = const_cast<agent::CommandRegistry*>(&registry);
    const auto status = registry.dispatch(line, ctx);
    if (captured) *captured = output.str();
    return status == agent::CommandStatus::Handled ||
           status == agent::CommandStatus::Failed;
}

TEST_CASE(command_registry_dispatches_known_command) {
    agent::CommandRegistry registry;
    registry.register_command({"hello",
        "says hello",
        "(no arguments)",
        false,
        [](const std::string&, agent::CommandContext& ctx) {
            if (ctx.output) *ctx.output << "hi\n";
            return agent::CommandStatus::Handled;
        }});
    std::string captured;
    REQUIRE(invoke(registry, "/hello", &captured));
    REQUIRE(captured == "hi\n");
}

TEST_CASE(command_registry_returns_not_mine_for_unknown) {
    agent::CommandRegistry registry;
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    agent::CommandContext ctx{&input, &output, &error, {}, nullptr};
    REQUIRE(registry.dispatch("/unknown", ctx) ==
            agent::CommandStatus::NotMine);
}

TEST_CASE(command_registry_returns_not_mine_for_non_slash) {
    agent::CommandRegistry registry;
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    agent::CommandContext ctx{&input, &output, &error, {}, nullptr};
    REQUIRE(registry.dispatch("hello world", ctx) ==
            agent::CommandStatus::NotMine);
}

TEST_CASE(command_registry_rejects_extra_args_for_no_arg_command) {
    agent::CommandRegistry registry;
    registry.register_command({"status",
        "show status",
        "(no arguments)",
        false,
        [](const std::string&, agent::CommandContext& ctx) {
            return agent::CommandStatus::Handled;
        }});
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    agent::CommandContext ctx{&input, &output, &error, {}, nullptr};
    REQUIRE(registry.dispatch("/status extra", ctx) ==
            agent::CommandStatus::Handled);
    REQUIRE(error.str().find("takes no arguments") != std::string::npos);
}

TEST_CASE(command_registry_extracts_argument) {
    agent::CommandRegistry registry;
    bool got_arg = false;
    std::string received;
    registry.register_command({"remember",
        "remember a fact",
        "<text>",
        true,
        [&](const std::string& argument, agent::CommandContext&) {
            received = argument;
            got_arg = true;
            return agent::CommandStatus::Handled;
        }});
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    agent::CommandContext ctx{&input, &output, &error, {}, nullptr};
    REQUIRE(registry.dispatch("/remember drink water", ctx) ==
            agent::CommandStatus::Handled);
    REQUIRE(got_arg);
    REQUIRE(received == "drink water");
}

TEST_CASE(command_registry_first_registration_wins) {
    agent::CommandRegistry registry;
    registry.register_command({"x", "first", "", false,
        [](const std::string&, agent::CommandContext&) {
            return agent::CommandStatus::Handled;
        }});
    registry.register_command({"x", "second", "", false,
        [](const std::string&, agent::CommandContext&) {
            return agent::CommandStatus::Failed;
        }});
    REQUIRE(registry.size() == 1u);
}

TEST_CASE(command_registry_snapshot_is_stable_order) {
    agent::CommandRegistry registry;
    registry.register_command({"a", "", "", false,
        [](const std::string&, agent::CommandContext&) {
            return agent::CommandStatus::Handled;
        }});
    registry.register_command({"b", "", "", false,
        [](const std::string&, agent::CommandContext&) {
            return agent::CommandStatus::Handled;
        }});
    const auto snap = registry.snapshot();
    REQUIRE(snap.size() == 2u);
    REQUIRE(snap[0].name == "a");
    REQUIRE(snap[1].name == "b");
}

TEST_CASE(command_registry_handles_throwing_handler) {
    agent::CommandRegistry registry;
    registry.register_command({"crash", "", "", false,
        [](const std::string&, agent::CommandContext&) -> agent::CommandStatus {
            throw std::runtime_error("boom");
        }});
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream error;
    agent::CommandContext ctx{&input, &output, &error, {}, nullptr};
    REQUIRE(registry.dispatch("/crash", ctx) ==
            agent::CommandStatus::Failed);
    REQUIRE(error.str().find("failed") != std::string::npos);
}

}  // namespace
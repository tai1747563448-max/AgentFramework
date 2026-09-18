#include "adapters/sandbox/sandbox_factory.h"
#include "test_support.h"

#include <string>

using agent::make_default_sandbox;
using agent::make_sandbox;

TEST_CASE(sandbox_factory_named_lookup_returns_a_live_sandbox) {
    REQUIRE(make_sandbox("none") != nullptr);
    REQUIRE(make_sandbox("none")->name() == "none");
    REQUIRE(make_sandbox("seatbelt") != nullptr);
    REQUIRE(make_sandbox("job_object") != nullptr);
    REQUIRE(make_sandbox("bwrap") != nullptr);
    REQUIRE(make_sandbox("nonsense") == nullptr);
}

TEST_CASE(sandbox_factory_default_is_non_null_on_every_platform) {
    const auto sandbox = make_default_sandbox();
    REQUIRE(sandbox != nullptr);
    const auto name = sandbox->name();
    const bool ok = name == "seatbelt" || name == "job_object" ||
                   name == "bwrap" || name == "none";
    REQUIRE(ok);
}

TEST_CASE(no_op_sandbox_passes_through_program_and_arguments) {
    const auto sandbox = make_sandbox("none");
    REQUIRE(sandbox != nullptr);
    agent::SandboxProfile profile;
    agent::SandboxedCommand out;
    const auto fault = sandbox->apply(
        profile, "/usr/bin/env", {"foo", "bar"}, "/workspace", out);
    REQUIRE(!fault.has_value());
    REQUIRE(out.program == "/usr/bin/env");
    REQUIRE(out.arguments.size() == 2);
    REQUIRE(out.arguments[0] == "foo");
    REQUIRE(out.arguments[1] == "bar");
}

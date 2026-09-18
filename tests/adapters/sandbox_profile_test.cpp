#include "ports/sandbox.h"
#include "test_support.h"

#include <map>
#include <string>

using agent::parse_sandbox_profile;
using agent::SandboxProfile;
using agent::serialise_sandbox_profile;

TEST_CASE(sandbox_profile_default_is_deny_by_default) {
    const SandboxProfile profile;
    REQUIRE(profile.deny_network);
    REQUIRE(profile.deny_writes_outside_workspace);
    REQUIRE(profile.read_paths.empty());
    REQUIRE(profile.write_paths.empty());
}

TEST_CASE(sandbox_profile_parses_explicit_overrides) {
    std::map<std::string, std::string> fields{
        {"deny_network", "false"},
        {"deny_writes_outside_workspace", "false"},
        {"read_paths", "/etc/passwd, /etc/hosts ,/usr/share"},
        {"write_paths", "/tmp/build, /tmp/cache"}};
    const auto profile = parse_sandbox_profile(fields);
    REQUIRE(!profile.deny_network);
    REQUIRE(!profile.deny_writes_outside_workspace);
    REQUIRE(profile.read_paths.size() == 3);
    REQUIRE(profile.read_paths[0] == "/etc/passwd");
    REQUIRE(profile.read_paths[1] == "/etc/hosts");
    REQUIRE(profile.read_paths[2] == "/usr/share");
    REQUIRE(profile.write_paths.size() == 2);
    REQUIRE(profile.write_paths[0] == "/tmp/build");
    REQUIRE(profile.write_paths[1] == "/tmp/cache");
}

TEST_CASE(sandbox_profile_round_trip_through_serialise) {
    SandboxProfile profile;
    profile.read_paths = {"/etc"};
    profile.write_paths = {"/var/log/agent"};
    auto serialised = serialise_sandbox_profile(profile);
    REQUIRE(serialised.at("deny_network") == "true");
    REQUIRE(serialised.at("deny_writes_outside_workspace") == "true");
    const auto parsed = parse_sandbox_profile(serialised);
    REQUIRE(parsed.deny_network == profile.deny_network);
    REQUIRE(parsed.deny_writes_outside_workspace ==
            profile.deny_writes_outside_workspace);
    REQUIRE(parsed.read_paths == profile.read_paths);
    REQUIRE(parsed.write_paths == profile.write_paths);
}

TEST_CASE(sandbox_profile_missing_fields_keep_defaults) {
    std::map<std::string, std::string> fields{{"read_paths", "/etc"}};
    const auto profile = parse_sandbox_profile(fields);
    REQUIRE(profile.deny_network);
    REQUIRE(profile.deny_writes_outside_workspace);
    REQUIRE(profile.read_paths.size() == 1);
}

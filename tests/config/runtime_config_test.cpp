#include "application/memory_policy.h"
#include "config/runtime_config.h"
#include "test_support.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace config_fixture {

class Environment final : public agent::Environment {
public:
    std::optional<std::string> get(const std::string& name) const override {
        const auto found = values.find(name);
        return found == values.end() ? std::nullopt :
            std::optional<std::string>{found->second};
    }
    std::map<std::string, std::string> values{
        {"AGENT_BASE_URL", "http://127.0.0.1:1"},
        {"AGENT_MODEL", "offline-fixture"},
        {"AGENT_API_KEY", "sixfixtureopaquevalue"}};
};

const std::vector<std::string> numeric_names{
    "AGENT_MEMORY_TOP_K", "AGENT_MEMORY_MAX_INJECTED_BYTES",
    "AGENT_MEMORY_MAX_ENTRY_BYTES", "AGENT_COMPACTION_THRESHOLD_BYTES",
    "AGENT_COMPACTION_HARD_LIMIT_BYTES", "AGENT_COMPACTION_RETAIN_TURNS",
    "AGENT_COMPACTION_MAX_SUMMARY_BYTES"};

} // namespace config_fixture

TEST_CASE(memory_config_rejects_nonexact_flags_without_echoing_values) {
    for (const auto& flag : {"", "true", "false", "01", "2", "-1", " 1", "1 "}) {
        config_fixture::Environment environment;
        environment.values["AGENT_ENABLE_MEMORY"] = flag;
        const auto loaded = agent::load_runtime_config(environment);
        REQUIRE(!loaded.has_value());
        REQUIRE(loaded.error().code == agent::ErrorCode::InvalidConfiguration);
        REQUIRE(loaded.error().message == "memory flag must be exactly 0 or 1");
    }
}

TEST_CASE(memory_config_rejects_malformed_numbers_even_when_memory_is_disabled) {
    for (const auto& name : config_fixture::numeric_names) {
        for (const auto& value : {"", "0", "-1", "+1", " 1", "1 ", "1x",
                                 "18446744073709551615", "18446744073709551616"}) {
            config_fixture::Environment environment;
            environment.values["AGENT_ENABLE_MEMORY"] = "0";
            environment.values[name] = value;
            const auto loaded = agent::load_runtime_config(environment);
            REQUIRE(!loaded.has_value());
            REQUIRE(loaded.error().code == agent::ErrorCode::InvalidConfiguration);
            REQUIRE(loaded.error().message ==
                    "memory and compaction numbers must be positive bounded integers");
        }
    }
}

TEST_CASE(memory_config_requires_hard_limit_strictly_above_threshold) {
    for (const auto& hard_limit : {"65535", "65536"}) {
        config_fixture::Environment environment;
        environment.values["AGENT_COMPACTION_HARD_LIMIT_BYTES"] = hard_limit;
        const auto loaded = agent::load_runtime_config(environment);
        REQUIRE(!loaded.has_value());
        REQUIRE(loaded.error().message ==
                "compaction hard limit must exceed threshold");
    }
}

TEST_CASE(memory_config_rejects_each_resource_cap_plus_one) {
    const std::vector<std::pair<std::string, std::string>> invalid{
        {"AGENT_MEMORY_TOP_K", "21"},
        {"AGENT_MEMORY_MAX_INJECTED_BYTES", "1048577"},
        {"AGENT_MEMORY_MAX_ENTRY_BYTES", "1048577"},
        {"AGENT_COMPACTION_THRESHOLD_BYTES", "16777217"},
        {"AGENT_COMPACTION_HARD_LIMIT_BYTES", "16777217"},
        {"AGENT_COMPACTION_RETAIN_TURNS", "10001"},
        {"AGENT_COMPACTION_MAX_SUMMARY_BYTES", "8193"}};
    for (const auto& value : invalid) {
        config_fixture::Environment environment;
        environment.values[value.first] = value.second;
        const auto loaded = agent::load_runtime_config(environment);
        REQUIRE(!loaded.has_value());
        REQUIRE(loaded.error().message ==
                "memory and compaction numbers must be positive bounded integers");
    }
}

TEST_CASE(memory_config_defaults_reach_context_and_protect_exact_provider_credential) {
    for (const auto* mode : {"AGENT_API_KEY", "AGENT_AUTH_TOKEN"}) {
        config_fixture::Environment environment;
        const auto protected_value = environment.values.at("AGENT_API_KEY");
        environment.values.erase("AGENT_API_KEY");
        environment.values[mode] = protected_value;
        const auto loaded = agent::load_runtime_config(environment);
        REQUIRE(loaded.has_value());
        const auto& config = loaded.value();
        REQUIRE(config.session_context.enabled);
        REQUIRE(config.session_context.memory_enabled);
        REQUIRE(config.session_context.memory_top_k == 5);
        REQUIRE(config.session_context.memory_max_injected_bytes == 4096);
        REQUIRE(config.session_context.threshold_bytes == 65536);
        REQUIRE(config.session_context.hard_limit_bytes == 131072);
        REQUIRE(config.session_context.retain_turns == 6);
        REQUIRE(config.session_context.max_summary_bytes == 8192);
        REQUIRE(config.memory_policy.max_entry_bytes == 1024);
        agent::MemoryPolicy policy(config.memory_policy);
        REQUIRE(policy.validate_candidate("C++ tests are offline").has_value());
        agent::MemoryPolicy without_protected_value({1024, {}});
        REQUIRE(without_protected_value.validate_candidate("remember " + protected_value).has_value());
        const auto unsafe = policy.validate_candidate("remember " + protected_value);
        REQUIRE(!unsafe.has_value());
        REQUIRE(unsafe.error().message.find(protected_value) == std::string::npos);
    }
}

TEST_CASE(memory_config_accepts_minimum_and_maximum_bounds_without_disabling_compaction) {
    for (const bool maximum : {false, true}) {
        config_fixture::Environment environment;
        environment.values["AGENT_ENABLE_MEMORY"] = maximum ? "1" : "0";
        environment.values["AGENT_MEMORY_TOP_K"] = maximum ? "20" : "1";
        environment.values["AGENT_MEMORY_MAX_INJECTED_BYTES"] = maximum ? "1048576" : "1";
        environment.values["AGENT_MEMORY_MAX_ENTRY_BYTES"] = maximum ? "1048576" : "1";
        environment.values["AGENT_COMPACTION_THRESHOLD_BYTES"] = maximum ? "16777215" : "1";
        environment.values["AGENT_COMPACTION_HARD_LIMIT_BYTES"] = maximum ? "16777216" : "2";
        environment.values["AGENT_COMPACTION_RETAIN_TURNS"] = maximum ? "10000" : "1";
        environment.values["AGENT_COMPACTION_MAX_SUMMARY_BYTES"] = maximum ? "8192" : "1";
        const auto loaded = agent::load_runtime_config(environment);
        REQUIRE(loaded.has_value());
        const auto& context = loaded.value().session_context;
        REQUIRE(context.enabled);
        REQUIRE(context.memory_enabled == maximum);
        REQUIRE(context.memory_top_k == (maximum ? 20U : 1U));
        REQUIRE(context.memory_max_injected_bytes == (maximum ? 1048576U : 1U));
        REQUIRE(loaded.value().memory_policy.max_entry_bytes == (maximum ? 1048576U : 1U));
        REQUIRE(context.threshold_bytes == (maximum ? 16777215U : 1U));
        REQUIRE(context.hard_limit_bytes == (maximum ? 16777216U : 2U));
        REQUIRE(context.retain_turns == (maximum ? 10000U : 1U));
        REQUIRE(context.max_summary_bytes == (maximum ? 8192U : 1U));
    }
    config_fixture::Environment environment;
    environment.values["AGENT_COMPACTION_THRESHOLD_BYTES"] = "16777216";
    environment.values["AGENT_COMPACTION_HARD_LIMIT_BYTES"] = "16777216";
    const auto at_cap = agent::load_runtime_config(environment);
    REQUIRE(!at_cap.has_value());
    REQUIRE(at_cap.error().message == "compaction hard limit must exceed threshold");
}

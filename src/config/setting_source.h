#pragma once

#include "domain/result.h"
#include "config/runtime_config.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace agent {

// SettingSource is the priority tag attached to each value resolved
// from a layer. The enum order matches the precedence used by
// LayeredSettings::resolve: Policy wins over Project wins over User
// wins over Env wins over BuiltIn. Adding a new tier means inserting
// it here and giving it a load helper.
enum class SettingSource {
    BuiltIn,
    Env,
    User,
    Project,
    Policy,
};

// SettingLayer is a flat key→string map produced by reading one of
// the supported config files (.agentrc.json / policy.json /
// user-config.json) or by walking the process environment. Keys use
// dot-notation (e.g. "anthropic.base_url", "budgets.max_model_rounds")
// so the loader does not need a schema-specific struct for every
// new key.
struct SettingLayer {
    SettingSource source{SettingSource::BuiltIn};
    std::string label;
    std::map<std::string, std::string> values;
};

// LayeredSettings holds the resolved layers in priority order
// (lowest first). resolve(key) walks the vector from back to front
// so the highest-priority layer that has the key wins.
class LayeredSettings {
public:
    void add_layer(SettingLayer layer) {
        layers_.push_back(std::move(layer));
    }
    const std::vector<SettingLayer>& layers() const { return layers_; }

    // Returns the first non-empty value found by walking layers from
    // highest precedence (last pushed) to lowest (first pushed).
    // Optional's second member is the source of the winning value.
    std::optional<std::pair<std::string, SettingSource>>
    resolve(const std::string& key) const;

    // Returns every layer that holds a non-empty value for the key,
    // ordered from highest precedence to lowest. Useful for
    // --show-effective-config to show what would have overridden what.
    std::vector<std::pair<std::string, SettingSource>>
    resolve_all(const std::string& key) const;

private:
    std::vector<SettingLayer> layers_;
};

// Reads a JSON config file and flattens its nested object structure
// into dot-notation keys. Non-object leaves become string values
// (numbers, booleans, strings all serialised via nlohmann::json::dump).
// Arrays are rejected because every latency config key maps to a
// single scalar. The strict parser (parse_strict_json) is used so
// duplicate keys and oversized inputs cannot sneak through.
Result<std::map<std::string, std::string>> read_agentrc_json(
    const std::filesystem::path& path);

// Converts a flat dot-notation map into a JSON object suitable for
// --show-effective-config output. The inverse of read_agentrc_json.
std::string render_layer_as_json(const SettingLayer& layer);

const char* setting_source_name(SettingSource source);

// The full set of well-known setting keys. The list keeps
// load_runtime_config's env→key mapping and the layered lookup in
// sync so a key added to one is added to the other.
const std::vector<std::string>& known_setting_keys();

// Build the 4-tier layered view (BuiltIn → Env → User → Project →
// Policy) by reading the env, the optional user config
// (%APPDATA%/agent/config.json or ~/.config/agent/config.json), the
// optional project config (cwd/.agentrc.json), and the optional
// admin policy (%ProgramData%/agent/policy.json or
// /etc/agent/policy.json). Missing files are silently skipped so
// the layered view degrades to env-only on minimal installations.
// The Env layer captures the values that the env has for the
// known_setting_keys() set; out-of-set keys are not surfaced.
Result<LayeredSettings> build_layered_settings(
    const Environment& environment,
    const std::filesystem::path& current_directory);

// Layered_environment adapts a LayeredSettings view into the
// Environment interface so existing helpers (nonempty /
// positive_integer / exact_flag / bounded_size) can be reused
// without change. The wrapper resolves each key through the layered
// view; missing keys return nullopt so the helpers fall back to
// their defaults.
class LayeredEnvironment final : public Environment {
public:
    explicit LayeredEnvironment(const LayeredSettings& settings)
        : settings_(&settings) {}
    std::optional<std::string> get(
        const std::string& name) const override;

private:
    const LayeredSettings* settings_;
};

// Render the LayeredSettings as a single JSON document. The output
// has one entry per layer in priority order, each with its source,
// label, and the flat key→value map for that layer. Suitable for
// `agent --show-effective-config`.
std::string render_layered_settings_json(
    const LayeredSettings& settings);

// Convenience: load process env via ProcessEnvironment and build
// the full LayeredSettings for the current working directory.
// Used by main.cpp to implement --show-effective-config.
Result<LayeredSettings> build_default_layered_settings();

}  // namespace agent

#include "config/setting_source.h"

#include "adapters/json/strict_json.h"
#include "config/runtime_config.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace agent {

const char* setting_source_name(SettingSource source) {
    switch (source) {
    case SettingSource::Policy:  return "policy";
    case SettingSource::Project: return "project";
    case SettingSource::User:    return "user";
    case SettingSource::Env:     return "env";
    case SettingSource::BuiltIn: return "builtin";
    }
    return "builtin";
}

std::optional<std::pair<std::string, SettingSource>>
LayeredSettings::resolve(const std::string& key) const {
    // Walk in reverse so the highest-priority layer wins. An
    // explicitly-set empty value is surfaced rather than skipped so
    // the strict validators (exact 0/1 flags, positive integers) can
    // reject it instead of silently falling back to a lower layer.
    for (auto it = layers_.rbegin(); it != layers_.rend(); ++it) {
        const auto found = it->values.find(key);
        if (found != it->values.end()) {
            return std::make_pair(found->second, it->source);
        }
    }
    return std::nullopt;
}

std::vector<std::pair<std::string, SettingSource>>
LayeredSettings::resolve_all(const std::string& key) const {
    std::vector<std::pair<std::string, SettingSource>> hits;
    for (auto it = layers_.rbegin(); it != layers_.rend(); ++it) {
        const auto found = it->values.find(key);
        if (found != it->values.end()) {
            hits.emplace_back(found->second, it->source);
        }
    }
    return hits;
}

namespace {

// Recursively flatten a JSON object into dot-notation keys. Only
// objects and scalar leaves are accepted; arrays produce a
// Result-failure because every latency config key maps to a
// single scalar value.
Result<void> flatten_json(const nlohmann::json& value,
                          const std::string& prefix,
                          std::map<std::string, std::string>& out) {
    if (value.is_object()) {
        if (value.empty()) {
            // An empty object {} is treated as a present-but-empty
            // value so resolve() can still surface it.
            out[prefix] = "{}";
            return Result<void>::success();
        }
        for (const auto& [k, v] : value.items()) {
            const std::string child = prefix.empty() ? k : prefix + "." + k;
            auto nested = flatten_json(v, child, out);
            if (!nested.has_value()) return nested;
        }
        return Result<void>::success();
    }
    if (value.is_null()) {
        out[prefix] = "";
        return Result<void>::success();
    }
    if (value.is_string()) {
        out[prefix] = value.get<std::string>();
        return Result<void>::success();
    }
    if (value.is_boolean()) {
        out[prefix] = value.get<bool>() ? "1" : "0";
        return Result<void>::success();
    }
    if (value.is_number_integer() || value.is_number_unsigned()) {
        out[prefix] = std::to_string(value.get<std::int64_t>());
        return Result<void>::success();
    }
    if (value.is_number_float()) {
        out[prefix] = std::to_string(value.get<double>());
        return Result<void>::success();
    }
    return Result<void>::failure(
        {ErrorCode::InvalidConfiguration,
         "agentrc config values must be scalars or objects", false});
}

}  // namespace

Result<std::map<std::string, std::string>> read_agentrc_json(
    const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        return Result<std::map<std::string, std::string>>::failure(
            {ErrorCode::InvalidConfiguration,
             "agentrc config file is not a regular file", false});
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return Result<std::map<std::string, std::string>>::failure(
            {ErrorCode::InvalidConfiguration,
             "agentrc config file could not be opened", false});
    }
    const std::string bytes{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
    if (bytes.empty()) {
        return Result<std::map<std::string, std::string>>::failure(
            {ErrorCode::InvalidConfiguration,
             "agentrc config file is empty", false});
    }
    constexpr std::uintmax_t kMaxBytes = 65'536;
    if (bytes.size() > kMaxBytes) {
        return Result<std::map<std::string, std::string>>::failure(
            {ErrorCode::InvalidConfiguration,
             "agentrc config file exceeds the 64 KiB limit", false});
    }
    const auto parsed = parse_strict_json(bytes, kMaxBytes);
    if (!parsed.value.has_value()) {
        return Result<std::map<std::string, std::string>>::failure(
            {ErrorCode::InvalidConfiguration,
             "agentrc config file failed strict parsing", false});
    }
    std::map<std::string, std::string> flat;
    auto flat_result = flatten_json(parsed.value.value(), "", flat);
    if (!flat_result.has_value()) {
        return Result<std::map<std::string, std::string>>::failure(
            flat_result.error());
    }
    return Result<std::map<std::string, std::string>>::success(
        std::move(flat));
}

namespace {

// Render the dot-notation flat map back into a nested JSON object
// by splitting each key on '.' and walking the tree.
nlohmann::json unflatten(const std::map<std::string, std::string>& flat) {
    nlohmann::json root = nlohmann::json::object();
    for (const auto& [k, v] : flat) {
        nlohmann::json* node = &root;
        std::string path = k;
        std::size_t pos = 0;
        while (true) {
            const auto dot = path.find('.', pos);
            if (dot == std::string::npos) {
                node->operator[](path.substr(pos)) = v;
                break;
            }
            const std::string segment = path.substr(pos, dot - pos);
            if (!node->contains(segment)) {
                node->operator[](segment) = nlohmann::json::object();
            }
            node = &node->operator[](segment);
            pos = dot + 1;
        }
    }
    return root;
}

}  // namespace

std::string render_layer_as_json(const SettingLayer& layer) {
    nlohmann::json envelope;
    envelope["source"] = setting_source_name(layer.source);
    envelope["label"] = layer.label;
    envelope["values"] = unflatten(layer.values);
    return envelope.dump(2);
}

const std::vector<std::string>& known_setting_keys() {
    static const std::vector<std::string> keys = {
        "AGENT_BASE_URL",
        "AGENT_MODEL",
        "AGENT_API_KEY",
        "AGENT_AUTH_TOKEN",
        "AGENT_MAX_TOKENS",
        "AGENT_MAX_MODEL_ROUNDS",
        "AGENT_MAX_TOOL_CALLS",
        "AGENT_MAX_PARALLEL_TOOLS",
        "AGENT_MAX_TASK_SECONDS",
        "AGENT_MODEL_TIMEOUT_SECONDS",
        "AGENT_ENABLE_BUILD_TOOLS",
        "AGENT_BUILD_TIMEOUT_SECONDS",
        "AGENT_ENABLE_RAG",
        "AGENT_ENABLE_MEMORY",
        "AGENT_MEMORY_TOP_K",
        "AGENT_MEMORY_MAX_INJECTED_BYTES",
        "AGENT_MEMORY_MAX_ENTRY_BYTES",
        "AGENT_COMPACTION_THRESHOLD_BYTES",
        "AGENT_COMPACTION_HARD_LIMIT_BYTES",
        "AGENT_COMPACTION_RETAIN_TURNS",
        "AGENT_COMPACTION_MAX_SUMMARY_BYTES",
        "AGENT_RUNTIME_ROOT",
        "AGENT_SYSTEM_PROMPT",
        "AGENT_RAG_MODE",
        "AGENT_RAG_DEVICE",
        "AGENT_RAG_RETRIEVAL_POLICY",
        "AGENT_RAG_TOP_K",
        "AGENT_RAG_MAX_TOTAL_BYTES",
        "AGENT_RAG_STARTUP_TIMEOUT_SECONDS",
        "AGENT_RAG_QUERY_TIMEOUT_SECONDS",
        "AGENT_RAG_PACK_ROOT",
        // T11: permission mode (default / acceptEdits / plan /
        // bypassPermissions / auto) and the JSON-encoded rule list
        // ({"tool":"x","decision":"allow|deny|ask"} entries).
        "AGENT_PERMISSION_MODE",
        "AGENT_PERMISSION_RULES",
    };
    return keys;
}

namespace {

// Resolve a config file path for a specific layer (User / Project
// / Policy). The function returns an empty path when the platform-
// specific convention points at a location that cannot be derived
// (no APPDATA on Windows, no HOME on Linux). Callers treat the empty
// path as "layer absent".
std::filesystem::path resolve_user_config_path() {
#if defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    if (appdata == nullptr || *appdata == '\0') return {};
    return std::filesystem::path(appdata) / "agent" / "config.json";
#else
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') return {};
    return std::filesystem::path(home) / ".config" / "agent" /
           "config.json";
#endif
}

std::filesystem::path resolve_policy_path() {
#if defined(_WIN32)
    const char* progdata = std::getenv("ProgramData");
    if (progdata == nullptr || *progdata == '\0') return {};
    return std::filesystem::path(progdata) / "agent" / "policy.json";
#else
    return std::filesystem::path("/etc/agent/policy.json");
#endif
}

}  // namespace

Result<LayeredSettings> build_layered_settings(
    const Environment& environment,
    const std::filesystem::path& current_directory) {
    LayeredSettings settings;

    // BuiltIn: no values yet. We could pre-populate defaults here, but
    // existing load_runtime_config already has hard-coded fallbacks
    // for every key, so leaving BuiltIn empty keeps the two code
    // paths in sync. Future keys with built-in defaults can register
    // them here without touching load_runtime_config.
    SettingLayer builtin;
    builtin.source = SettingSource::BuiltIn;
    builtin.label = "defaults";
    settings.add_layer(std::move(builtin));

    // Env: walk known_setting_keys so we surface the same set the
    // loader consumes, regardless of what the process env contains.
    // Explicitly-set empty values are preserved so the strict
    // validators (exact 0/1 flags, positive integers) can reject
    // them; silently dropping them here would mask config errors.
    SettingLayer env_layer;
    env_layer.source = SettingSource::Env;
    env_layer.label = "environment";
    for (const auto& key : known_setting_keys()) {
        const auto v = environment.get(key);
        if (v.has_value()) {
            env_layer.values[key] = *v;
        }
    }
    settings.add_layer(std::move(env_layer));

    // User: silently skip missing/unreadable files so the layered
    // view degrades to env-only on minimal installations.
    const auto user_path = resolve_user_config_path();
    if (!user_path.empty()) {
        auto user_result = read_agentrc_json(user_path);
        if (user_result.has_value()) {
            SettingLayer user_layer;
            user_layer.source = SettingSource::User;
            user_layer.label = user_path.u8string();
            user_layer.values = std::move(user_result.value());
            settings.add_layer(std::move(user_layer));
        }
    }

    // Project: cwd/.agentrc.json. Same skip-on-failure contract.
    const auto project_path = current_directory / ".agentrc.json";
    if (!project_path.empty()) {
        auto project_result = read_agentrc_json(project_path);
        if (project_result.has_value()) {
            SettingLayer project_layer;
            project_layer.source = SettingSource::Project;
            project_layer.label = project_path.u8string();
            project_layer.values = std::move(project_result.value());
            settings.add_layer(std::move(project_layer));
        }
    }

    // Policy: highest priority. Same skip-on-failure contract.
    const auto policy_path = resolve_policy_path();
    if (!policy_path.empty()) {
        auto policy_result = read_agentrc_json(policy_path);
        if (policy_result.has_value()) {
            SettingLayer policy_layer;
            policy_layer.source = SettingSource::Policy;
            policy_layer.label = policy_path.u8string();
            policy_layer.values = std::move(policy_result.value());
            settings.add_layer(std::move(policy_layer));
        }
    }

    return Result<LayeredSettings>::success(std::move(settings));
}

std::optional<std::string> LayeredEnvironment::get(
    const std::string& name) const {
    const auto resolved = settings_->resolve(name);
    if (!resolved.has_value()) return std::nullopt;
    return resolved->first;
}

std::string render_layered_settings_json(const LayeredSettings& settings) {
    nlohmann::json root = nlohmann::json::object();
    // Emit layers in priority order (highest precedence first) so
    // the human reader sees policy at the top of the report.
    auto& layers = root["layers"] = nlohmann::json::array();
    for (auto it = settings.layers().rbegin();
         it != settings.layers().rend(); ++it) {
        nlohmann::json entry;
        entry["source"] = setting_source_name(it->source);
        entry["label"] = it->label;
        entry["values"] = unflatten(it->values);
        layers.push_back(std::move(entry));
    }
    nlohmann::json resolved = nlohmann::json::object();
    for (const auto& key : known_setting_keys()) {
        const auto hits = settings.resolve_all(key);
        nlohmann::json entry = nlohmann::json::array();
        for (const auto& [value, source] : hits) {
            entry.push_back({{"value", value},
                             {"source", setting_source_name(source)}});
        }
        if (!entry.empty()) {
            resolved[key] = std::move(entry);
        }
    }
    root["resolved"] = std::move(resolved);
    return root.dump(2);
}

Result<LayeredSettings> build_default_layered_settings() {
    ProcessEnvironment env;
    std::error_code error;
    const auto cwd = std::filesystem::current_path(error);
    if (error) {
        return Result<LayeredSettings>::failure(
            {ErrorCode::InvalidConfiguration,
             "current working directory is unavailable", false});
    }
    return build_layered_settings(env, cwd);
}

}  // namespace agent

#include "config/runtime_config.h"
#include "test_support.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fixtures {

class Environment final : public agent::Environment {
public:
    std::optional<std::string> get(const std::string& name) const override {
        const auto found = values.find(name);
        return found == values.end() ? std::nullopt
                                     : std::optional<std::string>(found->second);
    }

    std::map<std::string, std::string> values{
        {"AGENT_BASE_URL", "https://provider.invalid"},
        {"AGENT_MODEL", "MiniMax-M3"},
        {"AGENT_API_KEY", "test-only-credential"}};
};

std::filesystem::path make_pack(const test::ScopedTempDir& temp,
                                const std::filesystem::path& relative = "pack") {
    const auto root = temp.path() / relative;
    std::filesystem::create_directories(root / "runtime");
    std::filesystem::create_directories(root / "sidecar");
    temp.write_text(relative / "runtime" / "python.exe", "fixture");
    temp.write_text(relative / "sidecar" / "agent_rag_cli.py", "fixture");
    temp.write_text(
        relative / "pack.json",
        nlohmann::json(
            {{"schema_version", 2},
             {"pack_id", "pack-cccccccccccccccccccccccccccccccc"},
             {"snapshot_date", "2026-09-03"},
             {"document_count", 30'000},
             {"chunk_count", 45'000},
             {"embedding_model", "BAAI/bge-m3"},
             {"embedding_revision", "5617a9f61b028005a4858fdac845db406aefb181"},
             {"embedding_dimensions", 1024},
             {"relevance_dense_min", 0.1},
             {"complete", true},
             {"files", nlohmann::json::array()}})
            .dump());
    return std::filesystem::canonical(root);
}

Environment enabled(const std::filesystem::path& root) {
    Environment environment;
    environment.values["AGENT_ENABLE_RAG"] = "1";
    environment.values["AGENT_RAG_PACK_ROOT"] = root.generic_u8string();
    return environment;
}

}  // namespace fixtures

TEST_CASE(rag_pack_config_defaults_are_complete_and_external) {
    test::ScopedTempDir temp("rag-config-defaults");
    const auto root = fixtures::make_pack(temp);
    const auto loaded = agent::load_runtime_config(fixtures::enabled(root));

    if (!loaded.has_value()) {
        throw std::runtime_error(loaded.error().message);
    }
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value().rag_enabled);
    REQUIRE(loaded.value().rag.enabled);
    REQUIRE(loaded.value().rag.mode == "hybrid");
    REQUIRE(loaded.value().rag.pack_root == root);
    REQUIRE(loaded.value().rag.top_k == 6);
    REQUIRE(loaded.value().rag.max_total_bytes == 32'768);
    REQUIRE(loaded.value().rag.startup_timeout_ms == 120'000);
    REQUIRE(loaded.value().rag.query_timeout_ms == 30'000);
    REQUIRE(loaded.value().rag.device == "auto");
}

TEST_CASE(rag_pack_config_parses_every_explicit_setting) {
    test::ScopedTempDir temp("rag-config-explicit");
    const auto root = fixtures::make_pack(temp);
    auto environment = fixtures::enabled(root);
    environment.values["AGENT_RAG_MODE"] = "lexical";
    environment.values["AGENT_RAG_TOP_K"] = "17";
    environment.values["AGENT_RAG_MAX_TOTAL_BYTES"] = "12345";
    environment.values["AGENT_RAG_STARTUP_TIMEOUT_SECONDS"] = "45";
    environment.values["AGENT_RAG_QUERY_TIMEOUT_SECONDS"] = "12";
    environment.values["AGENT_RAG_DEVICE"] = "cpu";
    const auto loaded = agent::load_runtime_config(environment);
    if (!loaded.has_value()) {
        throw std::runtime_error(loaded.error().message);
    }
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value().rag.mode == "lexical");
    REQUIRE(loaded.value().rag.top_k == 17);
    REQUIRE(loaded.value().rag.max_total_bytes == 12'345);
    REQUIRE(loaded.value().rag.startup_timeout_ms == 45'000);
    REQUIRE(loaded.value().rag.query_timeout_ms == 12'000);
    REQUIRE(loaded.value().rag.device == "cpu");
}

TEST_CASE(rag_pack_config_rejects_relative_incomplete_and_ready_internal_roots) {
    fixtures::Environment relative;
    relative.values["AGENT_ENABLE_RAG"] = "1";
    relative.values["AGENT_RAG_PACK_ROOT"] = "relative-pack";
    auto loaded = agent::load_runtime_config(relative);
    REQUIRE(!loaded.has_value());
    REQUIRE(loaded.error().message == "AGENT_RAG_PACK_ROOT must be absolute");

    test::ScopedTempDir temp("rag-config-paths");
    const auto incomplete = temp.path() / "incomplete";
    std::filesystem::create_directory(incomplete);
    loaded = agent::load_runtime_config(fixtures::enabled(incomplete));
    REQUIRE(!loaded.has_value());
    REQUIRE(loaded.error().message == "rag knowledge pack is unavailable");

    const auto inside_ready = fixtures::make_pack(
        temp, std::filesystem::path("out") / "AgentFramework-Ready" / "pack");
    loaded = agent::load_runtime_config(fixtures::enabled(inside_ready));
    REQUIRE(!loaded.has_value());
    REQUIRE(loaded.error().message == "rag knowledge pack must be external");
}

TEST_CASE(rag_pack_config_rejects_invalid_enums_numbers_without_echoing_values) {
    test::ScopedTempDir temp("rag-config-invalid");
    const auto root = fixtures::make_pack(temp);
    const std::vector<std::pair<std::string, std::string>> invalid{
        {"AGENT_RAG_MODE", "dense"},
        {"AGENT_RAG_DEVICE", "gpu-secret-value"},
        {"AGENT_RAG_TOP_K", "21"},
        {"AGENT_RAG_MAX_TOTAL_BYTES", "32769"},
        {"AGENT_RAG_STARTUP_TIMEOUT_SECONDS", "601"},
        {"AGENT_RAG_QUERY_TIMEOUT_SECONDS", "121"}};
    for (const auto& entry : invalid) {
        auto environment = fixtures::enabled(root);
        environment.values[entry.first] = entry.second;
        const auto loaded = agent::load_runtime_config(environment);
        REQUIRE(!loaded.has_value());
        REQUIRE(loaded.error().code == agent::ErrorCode::InvalidConfiguration);
        REQUIRE(loaded.error().message.find(entry.second) == std::string::npos);
    }
}

TEST_CASE(rag_pack_config_ignores_all_detail_settings_when_disabled) {
    fixtures::Environment environment;
    environment.values["AGENT_ENABLE_RAG"] = "0";
    environment.values["AGENT_RAG_MODE"] = "invalid";
    environment.values["AGENT_RAG_PACK_ROOT"] = "relative";
    environment.values["AGENT_RAG_TOP_K"] = "invalid";
    REQUIRE(agent::load_runtime_config(environment).has_value());
}

TEST_CASE(rag_pack_discovery_is_executable_relative_not_cwd_relative) {
    test::ScopedTempDir temp("rag-pack-discovery");
    const auto pack = fixtures::make_pack(temp, "external-pack");
    const auto deployment = temp.path() / "deployment";
    const auto pointers = temp.path() / "AgentFramework-Knowledge";
    std::filesystem::create_directories(deployment);
    std::filesystem::create_directories(pointers);
    const auto executable = temp.write_text(
        std::filesystem::path("deployment") / "AgentFramework.exe", "fixture");
    temp.write_text(
        std::filesystem::path("AgentFramework-Knowledge") / "active-pack.json",
        nlohmann::json({{"schema_version", 1},
                        {"pack_root", pack.generic_u8string()}})
            .dump());

    const auto discovered = agent::discover_rag_pack_root(executable);
    if (!discovered.has_value()) {
        throw std::runtime_error(discovered.error().message);
    }
    REQUIRE(discovered.has_value());
    REQUIRE(discovered.value().has_value());
    REQUIRE(discovered.value().value() == pack);
}

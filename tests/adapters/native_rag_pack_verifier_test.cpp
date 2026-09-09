#include "adapters/rag/native_rag_pack_verifier.h"
#include "adapters/workspace/workspace_text.h"
#include "test_support.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fixtures {

nlohmann::json record(const std::string& path, const std::string& bytes) {
    return {{"path", path},
            {"bytes", static_cast<std::uint64_t>(bytes.size())},
            {"sha256", agent::workspace::sha256_hex(bytes)}};
}

std::filesystem::path valid_pack(test::ScopedTempDir& temp) {
    const std::string python = "synthetic python executable";
    const std::string module = "print('trusted module')\n";
    const std::string sidecar = "print('trusted sidecar')\n";
    temp.write_text("runtime/python.exe", python);
    temp.write_text("runtime/module.py", module);
    temp.write_text("sidecar/agent_rag_cli.py", sidecar);
    const std::string intent = "{\"fixture\":true}";
    temp.write_text("build.intent.json", intent);

    const auto runtime_records = nlohmann::json::array(
        {record("runtime/module.py", module),
         record("runtime/python.exe", python)});
    const auto lock = nlohmann::json{
        {"schema_version", 2},
        {"intent_sha256", std::string(64, 'a')},
        {"python", {{"version", "fixture"}}},
        {"requirements_sha256", std::string(64, 'b')},
        {"wheels", nlohmann::json::array()},
        {"files", runtime_records}};
    const auto lock_bytes = lock.dump();
    temp.write_text("runtime.lock.json", lock_bytes);

    auto manifest_records = runtime_records;
    manifest_records.push_back(record("runtime.lock.json", lock_bytes));
    manifest_records.push_back(record("build.intent.json", intent));
    manifest_records.push_back(record("sidecar/agent_rag_cli.py", sidecar));
    const auto manifest = nlohmann::json{
        {"schema_version", 2},
        {"pack_id", "pack-cccccccccccccccccccccccccccccccc"},
        {"snapshot_date", "2026-09-03"},
        {"document_count", 30'000},
        {"chunk_count", 45'000},
        {"embedding_model", "BAAI/bge-m3"},
        {"embedding_revision", std::string(40, 'd')},
        {"embedding_dimensions", 1'024},
        {"relevance_dense_min", 0.61},
        {"complete", true},
        {"files", manifest_records}};
    temp.write_text("pack.json", manifest.dump());
    return temp.path();
}

std::string manifest_sha256(const std::filesystem::path& root) {
    std::ifstream input(root / "pack.json", std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
    return agent::workspace::sha256_hex(bytes);
}

}  // namespace fixtures

TEST_CASE(native_rag_pack_verifier_hashes_all_runtime_and_sidecar_files) {
    test::ScopedTempDir temp("native-rag-pack");
    const auto root = fixtures::valid_pack(temp);
    std::vector<std::string> progress;
    agent::NativeRagPackVerifier verifier(
        [&](const std::string& message) { progress.push_back(message); },
        fixtures::manifest_sha256(root));

    const auto result = verifier.verify_executable_payload(root);

    REQUIRE(result.has_value());
    REQUIRE(progress.size() >= 2);
    REQUIRE(progress.front().find("RAG bootstrap-integrity running: 0/3") == 0);
    REQUIRE(progress.back().find("RAG bootstrap-integrity completed: 3/3") == 0);
    REQUIRE(progress.back().find("throughput") != std::string::npos);
}

TEST_CASE(native_rag_pack_verifier_rejects_changed_or_unlisted_code) {
    test::ScopedTempDir temp("native-rag-pack-damaged");
    const auto root = fixtures::valid_pack(temp);
    agent::NativeRagPackVerifier verifier({}, fixtures::manifest_sha256(root));

    temp.write_text("runtime/module.py", "print('changed module')\n");
    auto changed = verifier.verify_executable_payload(root);
    REQUIRE(!changed.has_value());
    REQUIRE(changed.error().message ==
            "rag executable payload integrity check failed");

    fixtures::valid_pack(temp);
    temp.write_text("sidecar/unlisted.py", "print('unlisted')\n");
    const auto unlisted = verifier.verify_executable_payload(root);
    REQUIRE(!unlisted.has_value());
}

TEST_CASE(native_rag_pack_verifier_rejects_a_manifest_without_the_compiled_trust_root) {
    test::ScopedTempDir temp("native-rag-pack-untrusted-manifest");
    const auto root = fixtures::valid_pack(temp);
    agent::NativeRagPackVerifier verifier;

    const auto result = verifier.verify_executable_payload(root);

    REQUIRE(!result.has_value());
    REQUIRE(result.error().message ==
            "rag executable payload integrity check failed");
}

TEST_CASE(native_rag_pack_verifier_holds_verified_payload_read_only) {
#if defined(_WIN32)
    test::ScopedTempDir temp("native-rag-pack-locked");
    const auto root = fixtures::valid_pack(temp);
    agent::NativeRagPackVerifier verifier({}, fixtures::manifest_sha256(root));
    REQUIRE(verifier.verify_executable_payload(root).has_value());

    std::ofstream overwrite(root / "runtime" / "module.py",
                            std::ios::binary | std::ios::trunc);

    REQUIRE(!overwrite);
#endif
}

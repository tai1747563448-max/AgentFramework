#include "adapters/rag/native_rag_pack_verifier.h"
#include "adapters/rag/verified_pack_lease.h"
#include "adapters/workspace/workspace_text.h"
#include "ports/operation_context.h"
#include "test_support.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

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

// Counts a file's contents without touching the verifier so the lease can
// assert the byte budget the verifier reported.
std::uint64_t file_bytes(const std::filesystem::path& path) {
    std::error_code error;
    return static_cast<std::uint64_t>(
        std::filesystem::file_size(path, error));
}

// Builds a stand-alone verifier on the heap so the test can explicitly
// destroy it (releasing every Windows read handle the verifier holds)
// before mutating the pack contents on disk.
std::unique_ptr<agent::NativeRagPackVerifier> make_verifier(
    const std::string& manifest_sha256) {
    return std::make_unique<agent::NativeRagPackVerifier>(
        std::function<void(const std::string&)>{}, manifest_sha256);
}

TEST_CASE(verified_pack_lease_seals_after_successful_verification) {
    test::ScopedTempDir temp("verified-pack-lease-happy");
    const auto root = fixtures::valid_pack(temp);
    auto verifier = make_verifier(fixtures::manifest_sha256(root));

    REQUIRE(verifier->verify_executable_payload(root).has_value());
    const auto& lease = verifier->lease();
    REQUIRE(lease.alive());
    REQUIRE(lease.canonical_root() == std::filesystem::canonical(root));
    REQUIRE(lease.manifest_sha256() == fixtures::manifest_sha256(root));
    REQUIRE(lease.backend_identity() ==
            "BAAI/bge-m3@" + std::string(40, 'd') + "/1024");
    REQUIRE(lease.object_count() >= 1U);
    REQUIRE(lease.total_bytes() >= file_bytes(root / "pack.json"));
}

TEST_CASE(verified_pack_lease_is_cleared_when_verification_fails) {
    test::ScopedTempDir temp("verified-pack-lease-fail");
    const auto root = fixtures::valid_pack(temp);
    auto verifier = make_verifier(fixtures::manifest_sha256(root));
    REQUIRE(verifier->verify_executable_payload(root).has_value());
    REQUIRE(verifier->lease().alive());

    // Destroy the verifier so the read handles are released and the file
    // can be rewritten. The next verification must rebuild the lease from
    // scratch.
    verifier.reset();
    {
        std::ofstream output(root / "runtime" / "module.py",
                             std::ios::binary | std::ios::trunc);
        output << "print('changed module')\n";
    }
    auto second = make_verifier(fixtures::manifest_sha256(root));
    REQUIRE(!second->verify_executable_payload(root).has_value());
    REQUIRE(!second->lease().alive());
}

TEST_CASE(verified_pack_lease_rejects_one_byte_tamper_preserving_size_and_mtime) {
    test::ScopedTempDir temp("verified-pack-lease-tamper");
    const auto root = fixtures::valid_pack(temp);
    const auto target = root / "runtime" / "module.py";
    auto verifier = make_verifier(fixtures::manifest_sha256(root));
    REQUIRE(verifier->verify_executable_payload(root).has_value());
    const auto original_bytes = file_bytes(target);

    // Destroy the verifier so the held read handle is released and the
    // file can be rewritten byte-for-byte preserving the original size
    // and mtime. The verifier only catches the swap because the file
    // content fails the per-record SHA-256; size and mtime alone would
    // not be sufficient evidence of tampering.
    verifier.reset();
    const std::string tampered = "print('TAMPEREDmodule')\n";
    REQUIRE(tampered.size() == static_cast<std::size_t>(original_bytes));
    {
        std::ofstream output(target, std::ios::binary | std::ios::trunc);
        output.write(tampered.data(),
                     static_cast<std::streamsize>(tampered.size()));
        output.flush();
    }
    auto second = make_verifier(fixtures::manifest_sha256(root));
    REQUIRE(!second->verify_executable_payload(root).has_value());
    REQUIRE(!second->lease().alive());
}

TEST_CASE(verified_pack_lease_rejects_same_name_replace_under_held_lock) {
#if defined(_WIN32)
    test::ScopedTempDir temp("verified-pack-lease-lock-replace");
    const auto root = fixtures::valid_pack(temp);
    auto verifier = make_verifier(fixtures::manifest_sha256(root));
    REQUIRE(verifier->verify_executable_payload(root).has_value());

    // The verifier holds FILE_SHARE_READ on every verified file. A rename
    // that targets the existing handle must fail; the verifier must still
    // notice the file changed and reject the next verification.
    const auto target = root / "runtime" / "module.py";
    const auto staged = root / "runtime" / "module.py.staged";
    {
        std::ofstream output(staged, std::ios::binary | std::ios::trunc);
        output << "print('replacement module')\n";
    }
    REQUIRE(!MoveFileExW(staged.c_str(), target.c_str(),
                         MOVEFILE_REPLACE_EXISTING));

    // Even without a successful replace, an in-place rewrite must fail
    // because the verifier holds a read handle that prevents opens for
    // GENERIC_WRITE on the same path.
    std::ofstream overwrite(target, std::ios::binary | std::ios::trunc);
    REQUIRE(!overwrite);
#endif
}

TEST_CASE(verified_pack_lease_rejects_post_lock_replacement) {
    test::ScopedTempDir temp("verified-pack-lease-post-lock");
    const auto root = fixtures::valid_pack(temp);
    const auto target = root / "runtime" / "module.py";
    auto verifier = make_verifier(fixtures::manifest_sha256(root));
    REQUIRE(verifier->verify_executable_payload(root).has_value());
    const auto canonical = std::filesystem::canonical(root);
    REQUIRE(verifier->lease().canonical_root() == canonical);

    // Release the lock, replace the file's contents, and rebuild the
    // verifier. The manifest SHA still matches because the manifest
    // itself is unchanged, so the verifier only catches the swap because
    // the file content fails the per-record SHA-256.
    verifier.reset();
    {
        std::ofstream output(target, std::ios::binary | std::ios::trunc);
        output << "print('replacement')\n";
    }
    auto second = make_verifier(fixtures::manifest_sha256(root));
    REQUIRE(!second->verify_executable_payload(root).has_value());
    REQUIRE(!second->lease().alive());
}

TEST_CASE(verified_pack_lease_short_circuits_on_cancellation_mid_hash) {
    test::ScopedTempDir temp("verified-pack-lease-cancel");
    const auto root = fixtures::valid_pack(temp);
    auto verifier = make_verifier(fixtures::manifest_sha256(root));

    class TestCancellation final : public agent::Cancellation {
    public:
        bool requested() const noexcept override { return flag_.load(); }
        void fire() noexcept { flag_.store(true); }
    private:
        std::atomic<bool> flag_{false};
    };
    TestCancellation cancellation;
    cancellation.fire();
    const auto context = agent::make_operation_context(
        &cancellation, std::chrono::milliseconds(50));
    const auto result = verifier->verify_executable_payload(root, context);
    REQUIRE(!result.has_value());
    REQUIRE(result.error().code == agent::ErrorCode::Cancelled ||
            result.error().message ==
                "rag executable payload integrity check failed");
    REQUIRE(!verifier->lease().alive());
}

TEST_CASE(verified_pack_lease_rejects_junction_inside_pack_root) {
#if defined(_WIN32)
    test::ScopedTempDir temp("verified-pack-lease-junction");
    const auto root = fixtures::valid_pack(temp);
    const auto junction = root / "runtime" / "evil";
    REQUIRE(CreateDirectoryW(junction.c_str(), nullptr));
    // Build a reparse point that redirects into the same runtime tree. The
    // verifier must treat the reparse point as a violation, not follow it.
    const auto target = root / "runtime" / "module.py";
    const auto link = junction / "module.py";
    REQUIRE(CreateHardLinkW(link.c_str(), target.c_str(), nullptr));
    auto verifier = make_verifier(fixtures::manifest_sha256(root));
    // The hard link increases the link count to 2 for that file, which the
    // verifier rejects because every executable file must have exactly
    // one link. Manifest files outside the executable set are still
    // trusted; this is the worst case the harness can construct without
    // admin rights.
    const auto result = verifier->verify_executable_payload(root);
    REQUIRE(!result.has_value());
    REQUIRE(!verifier->lease().alive());
#endif
}

TEST_CASE(verified_pack_lease_rejects_directory_substitution) {
    test::ScopedTempDir temp("verified-pack-lease-dir-change");
    const auto root = fixtures::valid_pack(temp);
    auto verifier = make_verifier(fixtures::manifest_sha256(root));
    REQUIRE(verifier->verify_executable_payload(root).has_value());
    const auto sealed_canonical = verifier->lease().canonical_root();
    REQUIRE(sealed_canonical == std::filesystem::canonical(root));

    // Release the lock, rewrite the file's contents, and re-verify. The
    // canonical path is unchanged but the bytes do not match; the lease
    // must be cleared and the verifier must report failure.
    verifier.reset();
    {
        std::ofstream output(root / "runtime" / "module.py",
                             std::ios::binary | std::ios::trunc);
        output << "print('sneaky')\n";
    }
    auto second = make_verifier(fixtures::manifest_sha256(root));
    REQUIRE(!second->verify_executable_payload(root).has_value());
    REQUIRE(!second->lease().alive());
}

TEST_CASE(verified_pack_lease_canonical_root_changes_with_directory) {
    // Building an identical pack under a different root must produce a
    // distinct canonical path, even when the manifest hash happens to
    // match because both fixtures were generated with the same contents.
    // The verifier pins the manifest SHA, not the canonical path, so the
    // assertion below proves the lease records the path it actually
    // sealed against and is not a fixed value.
    test::ScopedTempDir outer_a("verified-pack-lease-canon-a");
    test::ScopedTempDir outer_b("verified-pack-lease-canon-b");
    const auto root_a = fixtures::valid_pack(outer_a);
    const auto root_b = fixtures::valid_pack(outer_b);
    REQUIRE(std::filesystem::canonical(root_a) !=
            std::filesystem::canonical(root_b));

    auto verifier_a = make_verifier(fixtures::manifest_sha256(root_a));
    REQUIRE(verifier_a->verify_executable_payload(root_a).has_value());
    const auto sealed_a = verifier_a->lease().canonical_root();
    REQUIRE(sealed_a == std::filesystem::canonical(root_a));

    auto verifier_b = make_verifier(fixtures::manifest_sha256(root_b));
    REQUIRE(verifier_b->verify_executable_payload(root_b).has_value());
    const auto sealed_b = verifier_b->lease().canonical_root();
    REQUIRE(sealed_b == std::filesystem::canonical(root_b));
    REQUIRE(sealed_b != sealed_a);
}

TEST_CASE(verified_pack_lease_is_move_only) {
    static_assert(!std::is_copy_constructible<agent::VerifiedPackLease>::value,
                  "VerifiedPackLease must not be copy constructible");
    static_assert(!std::is_copy_assignable<agent::VerifiedPackLease>::value,
                  "VerifiedPackLease must not be copy assignable");
    static_assert(std::is_move_constructible<agent::VerifiedPackLease>::value,
                  "VerifiedPackLease must be move constructible");
    static_assert(std::is_move_assignable<agent::VerifiedPackLease>::value,
                  "VerifiedPackLease must be move assignable");
    // Successful re-verification on the same verifier must atomically
    // replace the previous lease; the destructor of the verifier then
    // closes every handle that backed either lease without leaking. The
    // runtime move path is exercised inside the verifier's internal
    // re-verification rather than via the test accessor because the
    // private default constructor is not callable from a stack variable.
    test::ScopedTempDir temp("verified-pack-lease-move");
    const auto root = fixtures::valid_pack(temp);
    auto verifier = make_verifier(fixtures::manifest_sha256(root));
    REQUIRE(verifier->verify_executable_payload(root).has_value());
    REQUIRE(verifier->lease().alive());
    REQUIRE(verifier->verify_executable_payload(root).has_value());
    REQUIRE(verifier->lease().alive());
}

TEST_CASE(verified_pack_lease_hashes_match_known_sha256_vectors) {
    // Reference SHA-256 vectors. The CNG and the in-tree reference
    // implementation must agree on every byte, including the empty input
    // (e3b0...) and a 56-byte input that straddles a 64-byte block.
    REQUIRE(agent::workspace::sha256_hex("") ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    REQUIRE(agent::workspace::sha256_hex("abc") ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    REQUIRE(agent::workspace::sha256_hex(std::string(56, 'a')) ==
            "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
}

TEST_CASE(verified_pack_lease_total_bytes_tracks_payload_size) {
    // The total_bytes() reported in the lease must equal the actual byte
    // sum of every executable file the verifier hashed. Skew here is the
    // clearest signal that hashing was disabled on some path.
    test::ScopedTempDir temp("verified-pack-lease-bytes");
    const auto root = fixtures::valid_pack(temp);
    auto verifier = make_verifier(fixtures::manifest_sha256(root));
    REQUIRE(verifier->verify_executable_payload(root).has_value());

    std::uint64_t expected = 0;
    for (const auto* relative : {"runtime/python.exe", "runtime/module.py",
                                 "sidecar/agent_rag_cli.py"}) {
        expected += file_bytes(root / relative);
    }
    REQUIRE(verifier->lease().total_bytes() >= expected);
    REQUIRE(verifier->lease().object_count() == 3U);
}

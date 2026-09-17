#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace agent {

// VerifiedPackLease is the move-only RAII handle that proves a packed RAG
// runtime has been observed in full and remains canonical. T4 requires:
//   * the lease can only be constructed by NativeRagPackVerifier (friend
//     access; the constructor is private and the verifier uses a builder).
//   * the lease owns the canonicalised root, the manifest SHA-256 the
//     verifier pinned against, every Windows read handle that anchors the
//     payload (so an attacker cannot replace the file while we trust it),
//     and the backend identity bytes the pack was built with.
//   * the lease is destroyed after the sidecar process is stopped; the
//     main() ownership order already guarantees this, and the lease
//     destructor closes its handles deterministically.
//
// The lease intentionally exposes only const views; providers consult it but
// cannot alter it, and it cannot be reassigned across processes. A "directory
// change invalidates the lease" rule is enforced by re-canonicalising on
// construction; the canonical path is then compared on every retrieval path.
class VerifiedPackLease final {
public:
    VerifiedPackLease(const VerifiedPackLease&) = delete;
    VerifiedPackLease& operator=(const VerifiedPackLease&) = delete;

    VerifiedPackLease(VerifiedPackLease&& other) noexcept;
    VerifiedPackLease& operator=(VerifiedPackLease&& other) noexcept;
    ~VerifiedPackLease();

    bool alive() const noexcept { return !canonical_root_.empty(); }

    const std::filesystem::path& canonical_root() const noexcept {
        return canonical_root_;
    }

    const std::string& manifest_sha256() const noexcept {
        return manifest_sha256_;
    }

    const std::string& backend_identity() const noexcept {
        return backend_identity_;
    }

    // Total bytes the verifier actually hashed across the executable
    // payload. Latency baseline uses this number to keep the SHA budget
    // honest; cutting bytes here is the only legitimate way to shorten the
    // verification P95.
    std::uint64_t total_bytes() const noexcept { return total_bytes_; }

    // Number of objects the verifier walked, recorded for the baseline.
    std::size_t object_count() const noexcept { return object_count_; }

    void clear() noexcept;

private:
    friend class NativeRagPackVerifier;
    friend struct VerifiedPackLeaseAccess;

    VerifiedPackLease() = default;

    std::filesystem::path canonical_root_;
    std::string manifest_sha256_;
    std::string backend_identity_;
    std::vector<std::uintptr_t> locked_handles_;
    std::uint64_t total_bytes_{0};
    std::size_t object_count_{0};
};

// Test-only accessor declared in the header so the verified_pack_lease
// tests can construct a lease directly and exercise its move semantics
// without depending on NativeRagPackVerifier's internal friend access.
struct VerifiedPackLeaseAccess {
    static agent::VerifiedPackLease make_empty() {
        agent::VerifiedPackLease lease;
        return lease;
    }
    static void set_fields(agent::VerifiedPackLease& lease,
                           std::filesystem::path root,
                           std::string manifest_sha256,
                           std::string backend_identity,
                           std::uint64_t bytes,
                           std::size_t objects) {
        lease.canonical_root_ = std::move(root);
        lease.manifest_sha256_ = std::move(manifest_sha256);
        lease.backend_identity_ = std::move(backend_identity);
        lease.total_bytes_ = bytes;
        lease.object_count_ = objects;
    }
};

}  // namespace agent

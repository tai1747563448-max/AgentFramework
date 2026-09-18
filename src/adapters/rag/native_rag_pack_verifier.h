#pragma once

#include "ports/rag_pack_verifier.h"
#include "adapters/rag/verified_pack_lease.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace agent {

// NativeRagPackVerifier validates the executable portion of a published RAG
// pack against the manifest it was built with. T2 added OperationContext
// cancellation and deadline plumbing; T4 introduces a VerifiedPackLease that
// owns the canonicalised root, the manifest SHA-256, the file handles that
// anchor the payload, and the backend identity the lease was sealed with.
//
// The verifier can swap the SHA provider between the in-tree StreamingSha256
// (the historical reference implementation) and Windows CNG's
// BCryptCreateHash/BCryptHashData path. The old implementation is retained
// alongside the new one and the two are cross-checked against known vectors
// during verification so a behavioural regression cannot slip past. The
// default provider remains the reference implementation; the CNG path can
// be enabled via a constructor flag so the historical baseline and the
// performance-improved path produce identical results on the same bytes.
class NativeRagPackVerifier final : public RagPackVerifier {
public:
    enum class ShaProvider {
        Reference,
        // Windows CNG via BCryptCreateHash; chunked reads honour
        // OperationContext cancellation between chunks. Falls back to the
        // reference provider when bcrypt.dll cannot be initialised so the
        // contract is still met on non-Windows or restricted hosts.
        Cng,
    };

    explicit NativeRagPackVerifier(
        std::function<void(const std::string&)> progress_observer = {},
        std::string trusted_manifest_sha256 =
            "e3f13d34ef0e6958df56f114690afcfb21b34d6803df0957cff8c44ac9555ad0",
        ShaProvider sha_provider = ShaProvider::Cng);
    ~NativeRagPackVerifier() override;

    NativeRagPackVerifier(const NativeRagPackVerifier&) = delete;
    NativeRagPackVerifier& operator=(const NativeRagPackVerifier&) = delete;

    Result<void> verify_executable_payload(
        const std::filesystem::path& pack_root,
        const OperationContext& context) override;
    using RagPackVerifier::verify_executable_payload;

    // T4: read-only view of the lease established by the most recent
    // successful verification. Providers query this through the verifier to
    // learn the canonicalised root, manifest SHA, backend identity, and the
    // payload size the verifier committed to hashing. A subsequent call to
    // verify_executable_payload that fails clears the lease; successful
    // re-verification replaces it.
    const VerifiedPackLease& lease() const noexcept { return lease_; }

    ShaProvider sha_provider() const noexcept { return sha_provider_; }

private:
    void release_locks() noexcept;

    std::function<void(const std::string&)> progress_observer_;
    std::string trusted_manifest_sha256_;
    ShaProvider sha_provider_;
    VerifiedPackLease lease_;
};

}  // namespace agent

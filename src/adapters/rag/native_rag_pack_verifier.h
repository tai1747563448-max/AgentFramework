#pragma once

#include "ports/rag_pack_verifier.h"

#include <functional>
#include <cstdint>
#include <string>
#include <vector>

namespace agent {

class NativeRagPackVerifier final : public RagPackVerifier {
public:
    explicit NativeRagPackVerifier(
        std::function<void(const std::string&)> progress_observer = {},
        std::string trusted_manifest_sha256 =
            "5a355a489550451498317ae2245ce6cd29468f48ee98303d9bb9094c9e49707a");
    ~NativeRagPackVerifier() override;

    NativeRagPackVerifier(const NativeRagPackVerifier&) = delete;
    NativeRagPackVerifier& operator=(const NativeRagPackVerifier&) = delete;

    Result<void> verify_executable_payload(
        const std::filesystem::path& pack_root) override;

private:
    void release_locks() noexcept;

    std::function<void(const std::string&)> progress_observer_;
    std::string trusted_manifest_sha256_;
    std::vector<std::uintptr_t> locked_handles_;
};

}  // namespace agent

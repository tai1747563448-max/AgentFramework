#pragma once

#include "ports/rag_pack_verifier.h"

#include <functional>
#include <cstdint>
#include <chrono>
#include <string>
#include <vector>

namespace agent {

class NativeRagPackVerifier final : public RagPackVerifier {
public:
    explicit NativeRagPackVerifier(
        std::function<void(const std::string&)> progress_observer = {},
        std::string trusted_manifest_sha256 =
            "e3f13d34ef0e6958df56f114690afcfb21b34d6803df0957cff8c44ac9555ad0");
    ~NativeRagPackVerifier() override;

    NativeRagPackVerifier(const NativeRagPackVerifier&) = delete;
    NativeRagPackVerifier& operator=(const NativeRagPackVerifier&) = delete;

    Result<void> verify_executable_payload(
        const std::filesystem::path& pack_root,
        const OperationContext& context) override;

private:
    void release_locks() noexcept;

    std::function<void(const std::string&)> progress_observer_;
    std::string trusted_manifest_sha256_;
    std::vector<std::uintptr_t> locked_handles_;
};

}  // namespace agent

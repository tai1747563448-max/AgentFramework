#pragma once

#include "domain/result.h"

#include <filesystem>

namespace agent {

class RagPackVerifier {
public:
    virtual ~RagPackVerifier() = default;
    virtual Result<void> verify_executable_payload(
        const std::filesystem::path& pack_root) = 0;
};

}  // namespace agent

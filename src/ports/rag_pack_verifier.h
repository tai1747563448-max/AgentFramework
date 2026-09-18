#pragma once

#include "domain/result.h"
#include "ports/operation_context.h"

#include <filesystem>

namespace agent {

class RagPackVerifier {
public:
    virtual ~RagPackVerifier() = default;
    // T2: verification accepts the operation context so the native verifier
    // can short-circuit on cancellation and honour the unified deadline.
    virtual Result<void> verify_executable_payload(
        const std::filesystem::path& pack_root,
        const OperationContext& context) = 0;
    Result<void> verify_executable_payload(
        const std::filesystem::path& pack_root) {
        return verify_executable_payload(pack_root, OperationContext{});
    }
};

}  // namespace agent

#include "adapters/rag/verified_pack_lease.h"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

#include <utility>

namespace agent {

VerifiedPackLease::VerifiedPackLease(VerifiedPackLease&& other) noexcept
    : canonical_root_(std::move(other.canonical_root_)),
      manifest_sha256_(std::move(other.manifest_sha256_)),
      backend_identity_(std::move(other.backend_identity_)),
      locked_handles_(std::move(other.locked_handles_)),
      total_bytes_(other.total_bytes_),
      object_count_(other.object_count_) {
    other.canonical_root_.clear();
    other.manifest_sha256_.clear();
    other.backend_identity_.clear();
    other.locked_handles_.clear();
    other.total_bytes_ = 0;
    other.object_count_ = 0;
}

VerifiedPackLease& VerifiedPackLease::operator=(VerifiedPackLease&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    clear();
    canonical_root_ = std::move(other.canonical_root_);
    manifest_sha256_ = std::move(other.manifest_sha256_);
    backend_identity_ = std::move(other.backend_identity_);
    locked_handles_ = std::move(other.locked_handles_);
    total_bytes_ = other.total_bytes_;
    object_count_ = other.object_count_;
    other.canonical_root_.clear();
    other.manifest_sha256_.clear();
    other.backend_identity_.clear();
    other.locked_handles_.clear();
    other.total_bytes_ = 0;
    other.object_count_ = 0;
    return *this;
}

VerifiedPackLease::~VerifiedPackLease() {
    clear();
}

void VerifiedPackLease::clear() noexcept {
#if defined(_WIN32)
    for (const auto value : locked_handles_) {
        if (value != 0) {
            CloseHandle(reinterpret_cast<HANDLE>(value));
        }
    }
#endif
    locked_handles_.clear();
    canonical_root_.clear();
    manifest_sha256_.clear();
    backend_identity_.clear();
    total_bytes_ = 0;
    object_count_ = 0;
}

}  // namespace agent

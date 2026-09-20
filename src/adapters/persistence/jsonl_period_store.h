#pragma once

#include "ports/period_store.h"

#include <filesystem>

namespace agent {

// T2 (stage 2): writes Period records as a JSON array under
// `<runtime_root>/periods/snapshot.json`. Atomic write via tmp+rename
// so a crashed write never leaves a half-file.
class JsonlPeriodStore final : public PeriodStore {
public:
    explicit JsonlPeriodStore(std::filesystem::path runtime_root);

    Result<void> write_all(const std::vector<Period>& periods) override;
    Result<std::vector<Period>> read_all() override;
    std::filesystem::path snapshot_path() const override;

private:
    std::filesystem::path runtime_root_;
};

}  // namespace agent

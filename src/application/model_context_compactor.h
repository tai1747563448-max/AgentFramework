#pragma once

#include "ports/context_compactor.h"

namespace agent {

class ModelClient;

struct ModelContextCompactorConfig {
    std::size_t max_summary_bytes{8192};
    std::int64_t timeout_ms{120'000};
};

class ModelContextCompactor final : public ContextCompactor {
public:
    ModelContextCompactor(ModelClient& model, ModelContextCompactorConfig config);
    Result<std::string> compact(const ContextCompactionInput& input) override;

private:
    ModelClient& model_;
    ModelContextCompactorConfig config_;
};

} // namespace agent

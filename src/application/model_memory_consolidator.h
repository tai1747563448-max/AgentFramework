#pragma once

#include "ports/memory_consolidator.h"

namespace agent {

class ModelClient;

struct ModelMemoryConsolidatorConfig {
    std::int64_t timeout_ms{120'000};
};

class ModelMemoryConsolidator final : public MemoryConsolidator {
public:
    ModelMemoryConsolidator(ModelClient& model, ModelMemoryConsolidatorConfig config);
    Result<std::vector<MemoryCandidate>> consolidate(
        const MemoryConsolidationInput& input) override;

private:
    ModelClient& model_;
    ModelMemoryConsolidatorConfig config_;
};

} // namespace agent

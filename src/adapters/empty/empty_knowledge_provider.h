#pragma once

#include "ports/knowledge_provider.h"

namespace agent {

class EmptyKnowledgeProvider final : public KnowledgeProvider {
public:
    Result<EvidencePack> retrieve(const TaskState& state,
                                 const OperationContext& context) override;
    using KnowledgeProvider::retrieve;
};

}  // namespace agent

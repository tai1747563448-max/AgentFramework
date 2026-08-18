#pragma once

#include "ports/knowledge_provider.h"

namespace agent {

class EmptyKnowledgeProvider final : public KnowledgeProvider {
public:
    Result<EvidencePack> retrieve(const TaskState& state) override;
};

}  // namespace agent

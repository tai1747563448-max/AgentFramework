#pragma once

#include "domain/result.h"
#include "domain/task_state.h"

namespace agent {

class KnowledgeProvider {
public:
    virtual ~KnowledgeProvider() = default;
    virtual Result<EvidencePack> retrieve(const TaskState& state) = 0;
};

}  // namespace agent

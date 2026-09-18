#pragma once

#include "domain/result.h"
#include "domain/task_state.h"
#include "ports/operation_context.h"

namespace agent {

class KnowledgeProvider {
public:
    virtual ~KnowledgeProvider() = default;
    // T2: callers must pass the operation context so the provider can
    // short-circuit on cancellation and honour the deadline. Existing
    // single-argument call sites continue to compile through the default
    // implementation that builds a context with the cancellation token
    // attached and a generous fallback deadline.
    virtual Result<EvidencePack> retrieve(const TaskState& state,
                                         const OperationContext& context) = 0;
    Result<EvidencePack> retrieve(const TaskState& state) {
        return retrieve(state, OperationContext{});
    }
};

}  // namespace agent

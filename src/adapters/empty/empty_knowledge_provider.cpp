#include "adapters/empty/empty_knowledge_provider.h"

namespace agent {

Result<EvidencePack> EmptyKnowledgeProvider::retrieve(const TaskState&,
                                                  const OperationContext&) {
    return Result<EvidencePack>::success({});
}

}  // namespace agent

#pragma once

#include "domain/result.h"
#include "domain/session_state.h"

namespace agent {

struct ContextCompactionInput {
    std::string previous_summary;
    std::vector<CommittedSessionTurn> turns;
};

class ContextCompactor {
public:
    virtual ~ContextCompactor() = default;
    virtual Result<std::string> compact(const ContextCompactionInput& input) = 0;
};

} // namespace agent

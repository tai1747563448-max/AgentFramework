#pragma once

#include <string>

namespace agent {

enum class ErrorCode {
    InvalidInput,
    InvalidConfiguration,
    PersistenceFailure,
    TransportFailure,
    RequestTimeout,
    HttpFailure,
    ProtocolFailure,
    DependencyUnavailable,
    InvalidTransition,
    BudgetExceeded,
    Cancelled
};

struct RuntimeError {
    ErrorCode code;
    std::string message;
    bool retryable;
};

}  // namespace agent

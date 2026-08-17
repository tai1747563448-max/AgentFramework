#pragma once

#include "domain/model_types.h"
#include "domain/result.h"

namespace agent {

class ModelClient {
public:
    virtual ~ModelClient() = default;
    virtual Result<ModelResponse> complete(const ModelRequest& request) = 0;
};

}  // namespace agent

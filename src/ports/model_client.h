#pragma once

#include "domain/model_types.h"
#include "domain/model_stream_event.h"
#include "domain/result.h"
#include "ports/cancellation.h"

namespace agent {

class ModelClient {
public:
    virtual ~ModelClient() = default;
    virtual Result<ModelResponse> complete(const ModelRequest& request) = 0;
    virtual Result<ModelResponse> complete(const ModelRequest& request,
                                           const ModelCallOptions& options) {
        if (options.cancellation && options.cancellation->requested()) {
            return Result<ModelResponse>::failure(
                {ErrorCode::Cancelled, "model request cancelled", false});
        }
        auto response = complete(request);
        if (options.cancellation && options.cancellation->requested()) {
            return Result<ModelResponse>::failure(
                {ErrorCode::Cancelled, "model request cancelled", false});
        }
        return response;
    }
};

}  // namespace agent

#pragma once

#include "adapters/anthropic/http_transport.h"

namespace agent {

class CprHttpTransport final : public HttpTransport {
public:
    Result<HttpResponse> post(const HttpRequest& request) override;
    Result<HttpResponse> post(const HttpRequest& request,
                              const Cancellation* cancellation) override;
    Result<HttpResponse> post_stream(const HttpRequest& request,
                                     const HttpChunkObserver& observer,
                                     const Cancellation* cancellation) override;
};

}  // namespace agent

#pragma once

#include "adapters/anthropic/http_transport.h"

namespace agent {

class CprHttpTransport final : public HttpTransport {
public:
    Result<HttpResponse> post(const HttpRequest& request) override;
};

}  // namespace agent

#pragma once

#include "domain/result.h"

#include <cstdint>
#include <map>
#include <string>

namespace agent {

struct HttpRequest {
    std::string url;
    std::map<std::string, std::string> headers;
    std::string body;
    std::int64_t timeout_ms;
};

struct HttpResponse {
    int status;
    std::string body;
    std::map<std::string, std::string> headers;
};

class HttpTransport {
public:
    virtual ~HttpTransport() = default;
    virtual Result<HttpResponse> post(const HttpRequest& request) = 0;
};

}  // namespace agent

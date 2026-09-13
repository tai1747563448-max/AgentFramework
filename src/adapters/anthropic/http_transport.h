#pragma once

#include "domain/result.h"
#include "ports/cancellation.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

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

using HttpChunkObserver = std::function<bool(std::string_view)>;

inline bool http_response_is_event_stream(const HttpResponse& response) {
    for (const auto& header : response.headers) {
        auto name = header.first;
        auto value = header.second;
        const auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
        std::transform(name.begin(), name.end(), name.begin(), lower);
        if (name != "content-type") continue;
        std::transform(value.begin(), value.end(), value.begin(), lower);
        value = value.substr(0, value.find(';'));
        const auto first = value.find_first_not_of(" \t\r\n");
        const auto last = value.find_last_not_of(" \t\r\n");
        return first != std::string::npos && value.substr(first, last - first + 1) == "text/event-stream";
    }
    return false;
}

class HttpTransport {
public:
    virtual ~HttpTransport() = default;
    virtual Result<HttpResponse> post(const HttpRequest& request) = 0;
    virtual Result<HttpResponse> post(const HttpRequest& request,
                                     const Cancellation* cancellation) {
        if (cancellation && cancellation->requested()) {
            return Result<HttpResponse>::failure(
                {ErrorCode::Cancelled, "HTTP request cancelled", false});
        }
        auto response = post(request);
        if (cancellation && cancellation->requested()) {
            return Result<HttpResponse>::failure(
                {ErrorCode::Cancelled, "HTTP request cancelled", false});
        }
        return response;
    }
    virtual Result<HttpResponse> post_stream(const HttpRequest& request,
                                            const HttpChunkObserver& observer,
                                            const Cancellation* cancellation) {
        auto response = post(request, cancellation);
        if (response.has_value() && response.value().status >= 200 &&
            response.value().status < 300 && http_response_is_event_stream(response.value())) {
            if (observer && !observer(response.value().body)) {
                return Result<HttpResponse>::failure(
                    {ErrorCode::ProtocolFailure, "HTTP stream consumer rejected data", false});
            }
            response.value().body.clear();
        }
        return response;
    }
};

}  // namespace agent

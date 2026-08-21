#include "adapters/anthropic/cpr_http_transport.h"

#include <cpr/cpr.h>

#include <chrono>
#include <map>
#include <string>
#include <utility>

namespace agent {

Result<HttpResponse> CprHttpTransport::post(const HttpRequest& request) {
    try {
        cpr::Header headers;
        for (const auto& header : request.headers) {
            headers.emplace(header.first, header.second);
        }

        const auto response = cpr::Post(
            cpr::Url{request.url}, std::move(headers), cpr::Body{request.body},
            cpr::Timeout{std::chrono::milliseconds(request.timeout_ms)},
            cpr::Redirect{false});

        if (response.error.code != cpr::ErrorCode::OK) {
            if (response.error.code == cpr::ErrorCode::OPERATION_TIMEDOUT) {
                return Result<HttpResponse>::failure(
                    {ErrorCode::RequestTimeout, "HTTP request timed out", true});
            }
            return Result<HttpResponse>::failure(
                {ErrorCode::TransportFailure, "HTTP transport failed", true});
        }

        std::map<std::string, std::string> response_headers;
        for (const auto& header : response.header) {
            response_headers.emplace(header.first, header.second);
        }
        return Result<HttpResponse>::success(
            {static_cast<int>(response.status_code), response.text,
             std::move(response_headers)});
    } catch (...) {
        return Result<HttpResponse>::failure(
            {ErrorCode::TransportFailure, "HTTP transport failed", true});
    }
}

}  // namespace agent

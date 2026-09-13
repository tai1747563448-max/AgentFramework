#include "adapters/anthropic/cpr_http_transport.h"

#include <cpr/cpr.h>

#include <chrono>
#include <charconv>
#include <map>
#include <string>
#include <utility>

namespace agent {
namespace {

Result<HttpResponse> perform(const HttpRequest& request,
                             const Cancellation* cancellation,
                             const HttpChunkObserver* observer) {
    const auto cancelled = [&] { return cancellation && cancellation->requested(); };
    const auto cancellation_error = [] {
        return Result<HttpResponse>::failure(
            {ErrorCode::Cancelled, "HTTP request cancelled", false});
    };
    if (cancelled()) return cancellation_error();
    try {
        cpr::Header headers;
        for (const auto& header : request.headers) headers.emplace(header.first, header.second);
        cpr::Session session;
        session.SetUrl(cpr::Url{request.url});
        session.SetHeader(headers);
        session.SetBody(cpr::Body{request.body});
        session.SetTimeout(cpr::Timeout{std::chrono::milliseconds(request.timeout_ms)});
        session.SetRedirect(cpr::Redirect{false});
        session.SetProgressCallback(cpr::ProgressCallback{
            [&](cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t, cpr::cpr_pf_arg_t,
                cpr::cpr_pf_arg_t, intptr_t) { return !cancelled(); }});

        HttpResponse streamed{0, {}, {}};
        std::size_t header_bytes = 0;
        bool consumer_failed = false;
        if (observer) {
            // A custom CPR header callback owns header collection as well.
            session.SetHeaderCallback(cpr::HeaderCallback{
                [&](std::string_view line, intptr_t) {
                    try {
                        if (cancelled()) return false;
                        if (line.size() > 64 * 1024 - header_bytes) {
                            consumer_failed = true;
                            return false;
                        }
                        header_bytes += line.size();
                        if (line.substr(0, 5) == "HTTP/") {
                            streamed.headers.clear();  // Discard interim 1xx/proxy headers.
                            const auto space = line.find(' ');
                            if (space == std::string_view::npos) return false;
                            const auto code = line.substr(space + 1, 3);
                            const auto parsed = std::from_chars(code.data(), code.data() + code.size(), streamed.status);
                            if (parsed.ec != std::errc{}) return false;
                        } else {
                            const auto colon = line.find(':');
                            if (colon != std::string_view::npos) {
                                auto name = std::string(line.substr(0, colon));
                                std::transform(name.begin(), name.end(), name.begin(),
                                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                                const auto first = line.find_first_not_of(" \t", colon + 1);
                                const auto last = line.find_last_not_of(" \t\r\n");
                                streamed.headers[name] = first == std::string_view::npos || last < first
                                    ? std::string{} : std::string(line.substr(first, last - first + 1));
                            }
                        }
                        return true;
                    } catch (...) {
                        consumer_failed = true;
                        return false;
                    }
                }});
            session.SetWriteCallback(cpr::WriteCallback{
                [&](std::string_view bytes, intptr_t) {
                    try {
                        if (cancelled()) return false;
                        if (streamed.status < 200 || streamed.status >= 300) return true;
                        if (http_response_is_event_stream(streamed)) {
                            if (*observer && !(*observer)(bytes)) {
                                consumer_failed = true;
                                return false;
                            }
                        } else {
                            // Some compatible providers ignore stream=true. Retain
                            // their bounded JSON response without inventing previews.
                            constexpr std::size_t limit = 8 * 1024 * 1024;
                            if (bytes.size() > limit - streamed.body.size()) {
                                consumer_failed = true;
                                return false;
                            }
                            streamed.body.append(bytes.data(), bytes.size());
                        }
                        return true;
                    } catch (...) {
                        consumer_failed = true;
                        return false;
                    }
                }});
        }
        const auto response = session.Post();
        if (cancelled()) return cancellation_error();
        if (consumer_failed) {
            return Result<HttpResponse>::failure(
                {ErrorCode::ProtocolFailure, "HTTP stream consumer rejected data", false});
        }
        if (response.error.code != cpr::ErrorCode::OK) {
            if (response.error.code == cpr::ErrorCode::OPERATION_TIMEDOUT) {
                return Result<HttpResponse>::failure(
                    {ErrorCode::RequestTimeout, "HTTP request timed out", true});
            }
            return Result<HttpResponse>::failure(
                {ErrorCode::TransportFailure, "HTTP transport failed", true});
        }
        if (observer) {
            streamed.status = static_cast<int>(response.status_code);
            return Result<HttpResponse>::success(std::move(streamed));
        }
        std::map<std::string, std::string> response_headers;
        for (const auto& header : response.header) response_headers.emplace(header.first, header.second);
        return Result<HttpResponse>::success(
            {static_cast<int>(response.status_code), response.text, std::move(response_headers)});
    } catch (...) {
        if (cancelled()) return cancellation_error();
        return Result<HttpResponse>::failure(
            {ErrorCode::TransportFailure, "HTTP transport failed", true});
    }
}

}  // namespace

Result<HttpResponse> CprHttpTransport::post(const HttpRequest& request) {
    return perform(request, nullptr, nullptr);
}

Result<HttpResponse> CprHttpTransport::post(const HttpRequest& request,
                                           const Cancellation* cancellation) {
    return perform(request, cancellation, nullptr);
}

Result<HttpResponse> CprHttpTransport::post_stream(const HttpRequest& request,
                                                  const HttpChunkObserver& observer,
                                                  const Cancellation* cancellation) {
    return perform(request, cancellation, &observer);
}

}  // namespace agent

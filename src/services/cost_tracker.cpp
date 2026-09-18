#include "services/cost_tracker.h"
#include "domain/runtime_error.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

namespace agent {
namespace {

constexpr std::size_t kBatchThreshold = 32;
constexpr std::chrono::milliseconds kBatchInterval{500};

double price_for_model(const std::string& model) {
    if (model == "claude-3-5-sonnet" || model == "claude-3-5-sonnet-latest" ||
        model == "claude-3-5-sonnet-20240620" ||
        model == "claude-3-5-sonnet-20241022") {
        // input $3 / M, output $15 / M.
        return 0.0;
    }
    if (model == "claude-3-opus" || model == "claude-3-opus-20240229") {
        return 0.0;
    }
    return 0.0;
}

std::string now_utc_iso8601() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &time);
#else
    gmtime_r(&time, &tm_buf);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return buffer;
}

std::string escape(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (const char byte : raw) {
        switch (byte) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(byte) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                              static_cast<unsigned char>(byte));
                out += buffer;
            } else {
                out.push_back(byte);
            }
        }
    }
    return out;
}

double cost_for(const std::string& model,
                std::size_t input_tokens,
                std::size_t output_tokens) {
    // We only track a handful of SKUs here. Anything else records a
    // 0 USD entry so the row stays present in the aggregate.
    if (model == "claude-3-5-sonnet" || model == "claude-3-5-sonnet-latest" ||
        model == "claude-3-5-sonnet-20240620" ||
        model == "claude-3-5-sonnet-20241022") {
        return input_tokens * 3.0 / 1'000'000.0 +
               output_tokens * 15.0 / 1'000'000.0;
    }
    if (model == "claude-3-opus" || model == "claude-3-opus-20240229") {
        return input_tokens * 15.0 / 1'000'000.0 +
               output_tokens * 75.0 / 1'000'000.0;
    }
    if (model == "claude-3-5-haiku" || model == "claude-3-5-haiku-latest" ||
        model == "claude-3-5-haiku-20241022") {
        return input_tokens * 1.0 / 1'000'000.0 +
               output_tokens * 5.0 / 1'000'000.0;
    }
    return 0.0;
}

}  // namespace

CostTrackerSink::CostTrackerSink(std::filesystem::path usage_path)
    : usage_path_(std::move(usage_path)),
      stream_(usage_path_,
              std::ios::out | std::ios::binary | std::ios::app) {
    if (!stream_.is_open()) {
        // Sink failures must never crash the runtime; the hook will
        // still call record() but writes will silently drop. Tests
        // can detect this by inspecting usage_path_'s directory.
    }
}

CostTrackerSink::~CostTrackerSink() {
    try { flush_locked(); } catch (...) {}
}

void CostTrackerSink::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    flush_locked();
}

void CostTrackerSink::flush_locked() {
    if (pending_ == 0) return;
    stream_ << buffer_;
    stream_.flush();
    buffer_.clear();
    pending_ = 0;
}

void CostTrackerSink::record(const std::string& task_id,
                             const std::string& model,
                             std::size_t input_tokens,
                             std::size_t output_tokens,
                             double usd,
                             bool success) {
    std::lock_guard<std::mutex> lock(mutex_);
    char number[64];
    std::snprintf(number, sizeof(number), "%.6f", usd);
    std::ostringstream row;
    row << "{\"ts\":\"" << escape(now_utc_iso8601()) << "\""
        << ",\"task_id\":\"" << escape(task_id) << "\""
        << ",\"model\":\"" << escape(model) << "\""
        << ",\"input\":" << input_tokens
        << ",\"output\":" << output_tokens
        << ",\"usd\":" << number
        << ",\"success\":" << (success ? "true" : "false")
        << "}\n";
    buffer_ += row.str();
    ++pending_;
    if (pending_ >= kBatchThreshold) {
        flush_locked();
    }
}

CostTrackerSink::Aggregate CostTrackerSink::read_aggregate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Aggregate out;
    std::ifstream input(usage_path_, std::ios::binary);
    if (!input) return out;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        std::size_t pos = 0;
        // Cheap parse: pull the integer / float fields out of the
        // known record shape so /cost does not need a JSON library
        // at runtime. The cost tracker is the only writer so the
        // shape is stable.
        auto find_int = [&](const std::string& key, std::size_t& value) {
            const auto needle = "\"" + key + "\":";
            const auto hit = line.find(needle);
            if (hit == std::string::npos) return;
            pos = hit + needle.size();
            std::size_t end = pos;
            while (end < line.size() && line[end] >= '0' && line[end] <= '9') {
                ++end;
            }
            if (end > pos) {
                value = std::stoull(line.substr(pos, end - pos));
            }
        };
        auto find_float = [&](const std::string& key, double& value) {
            const auto needle = "\"" + key + "\":";
            const auto hit = line.find(needle);
            if (hit == std::string::npos) return;
            pos = hit + needle.size();
            std::size_t end = pos;
            while (end < line.size() &&
                   (line[end] >= '0' && line[end] <= '9' || line[end] == '.')) {
                ++end;
            }
            if (end > pos) {
                value = std::stod(line.substr(pos, end - pos));
            }
        };
        std::size_t input = 0, output = 0;
        double usd = 0.0;
        find_int("input", input);
        find_int("output", output);
        find_float("usd", usd);
        ++out.calls;
        out.input_tokens += input;
        out.output_tokens += output;
        out.usd += usd;
    }
    return out;
}

PostModelCallHook make_cost_tracker_hook(
    CostTrackerSink& sink,
    std::function<std::string()> model_name) {
    return [&sink, model_name = std::move(model_name)](HookPostModelCall& event) {
        try {
            const auto& response = event.response;
            const std::string model = model_name ? model_name() : "unknown";
            sink.record(event.task_id, model,
                        response ? response->input_tokens : 0,
                        response ? response->output_tokens : 0,
                        cost_for(model,
                                 response ? response->input_tokens : 0,
                                 response ? response->output_tokens : 0),
                        response != nullptr);
        } catch (...) {
            // Cost tracking must never propagate; runtime would log it
            // and continue.
        }
    };
}

}  // namespace agent
#include "adapters/rag/persistent_rag_knowledge_provider.h"

#include "adapters/rag/rag_protocol.h"
#include "adapters/workspace/workspace_text.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

namespace agent {
namespace {

RuntimeError invalid_config() {
    return {ErrorCode::InvalidConfiguration, "invalid rag configuration", false};
}

RuntimeError invalid_query() {
    return {ErrorCode::InvalidInput, "invalid rag query", false};
}

RuntimeError fixed_process_error(const RuntimeError& error, bool startup) {
    if (error.code == ErrorCode::RequestTimeout) {
        return {ErrorCode::RequestTimeout,
                startup ? "rag startup timed out" : "rag query timed out", true};
    }
    if (error.code == ErrorCode::ProtocolFailure) {
        return {ErrorCode::ProtocolFailure, "rag process protocol failed", false};
    }
    return {ErrorCode::DependencyUnavailable, "rag process unavailable", true};
}

bool valid_config(const RagConfig& config) {
    return config.pack_root.is_absolute() &&
           (config.mode == "hybrid" || config.mode == "dense" ||
            config.mode == "lexical") &&
           config.top_k >= 1 && config.top_k <= 20 &&
           config.max_total_bytes >= 1 && config.max_total_bytes <= 32'768 &&
           config.startup_timeout_ms >= 1 && config.startup_timeout_ms <= 600'000 &&
           config.query_timeout_ms >= 1 && config.query_timeout_ms <= 600'000 &&
           (config.device == "auto" || config.device == "cpu" ||
            config.device == "cuda");
}

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char byte) {
        return static_cast<char>(std::tolower(byte));
    });
    return value;
}

bool should_skip(const std::string& issue) {
    const auto first = issue.find_first_not_of(" \t\r\n");
    if (first != std::string::npos && issue[first] == '/') {
        return true;
    }
    const auto lower = ascii_lower(issue);
    for (const auto* phrase : {"do not use rag", "don't use rag",
                               "do not use the knowledge base",
                               "don't use the knowledge base"}) {
        if (lower.find(phrase) != std::string::npos) {
            return true;
        }
    }
    for (const auto* phrase : {u8"不要查询知识库", u8"不要使用知识库",
                               u8"不要用知识库", u8"不要用 rag", u8"不要用rag",
                               u8"不使用 rag", u8"不使用rag"}) {
        if (lower.find(phrase) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

PersistentRagKnowledgeProvider::PersistentRagKnowledgeProvider(
    JsonlProcess& process, RagPackVerifier& pack_verifier, RagConfig config)
    : process_(process), pack_verifier_(pack_verifier),
      config_(std::move(config)) {}

PersistentRagKnowledgeProvider::~PersistentRagKnowledgeProvider() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::Stopped || process_.running()) {
        process_.stop(2'000);
    }
    state_ = State::Stopped;
}

Result<void> PersistentRagKnowledgeProvider::ensure_ready() {
    if (state_ == State::Ready && process_.running()) {
        return Result<void>::success();
    }
    if (state_ != State::Stopped) {
        process_.stop(1'000);
    }
    state_ = State::Starting;
    auto verified = pack_verifier_.verify_executable_payload(config_.pack_root);
    if (!verified.has_value()) {
        state_ = State::Broken;
        return verified;
    }
    const auto python = config_.pack_root / "runtime" / "python.exe";
    const auto script = config_.pack_root / "sidecar" / "agent_rag_cli.py";
    JsonlProcessRequest request;
    request.program = python.generic_u8string();
    request.arguments = {"-B", "-E", "-s", "-X", "utf8", script.generic_u8string(),
                         "serve", "--pack-root",
                         config_.pack_root.generic_u8string(), "--device",
                         config_.device};
    request.working_directory = script.parent_path();
    request.startup_timeout_ms = config_.startup_timeout_ms;
    request.max_stdout_line_bytes = 65'536;
    request.max_stderr_bytes = 4'096;
    auto started = process_.start(request);
    if (!started.has_value()) {
        state_ = State::Broken;
        return Result<void>::failure(fixed_process_error(started.error(), true));
    }
    auto ready = rag::decode_ready(started.value());
    if (!ready.has_value()) {
        break_process();
        return Result<void>::failure(ready.error());
    }
    retrieval_revision_ = ready.value().retrieval_revision;
    state_ = State::Ready;
    return Result<void>::success();
}

std::string PersistentRagKnowledgeProvider::next_request_id() {
    ++request_sequence_;
    std::ostringstream stream;
    stream << "req-" << std::hex << std::nouppercase << std::setfill('0')
           << std::setw(32) << request_sequence_;
    return stream.str();
}

void PersistentRagKnowledgeProvider::break_process() noexcept {
    state_ = State::Broken;
    retrieval_revision_.clear();
    process_.stop(1'000);
}

Result<EvidencePack> PersistentRagKnowledgeProvider::retrieve(
    const TaskState& state, const OperationContext& context) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (context.cancelled()) {
        return Result<EvidencePack>::failure(
            {ErrorCode::Cancelled, "RAG retrieval cancelled", false});
    }
    try {
        if (!config_.enabled || should_skip(state.issue)) {
            return Result<EvidencePack>::success({});
        }
        if (!valid_config(config_)) {
            return Result<EvidencePack>::failure(invalid_config());
        }
        if (state.issue.empty() || state.issue.size() > 16'384 ||
            state.issue.find('\0') != std::string::npos ||
            !workspace::is_strict_utf8_text(state.issue)) {
            return Result<EvidencePack>::failure(invalid_query());
        }
        auto ready = ensure_ready();
        if (!ready.has_value()) {
            return Result<EvidencePack>::failure(ready.error());
        }
        const auto request_id = next_request_id();
        const nlohmann::json request{
            {"schema_version", 2},
            {"request_id", request_id},
            {"op", "query"},
            {"payload",
             {{"query", state.issue},
              {"top_k", config_.top_k},
              {"max_total_bytes", config_.max_total_bytes},
              {"mode", config_.mode}}}};
        auto response = process_.exchange(request.dump(), config_.query_timeout_ms);
        if (!response.has_value()) {
            const auto error = fixed_process_error(response.error(), false);
            break_process();
            return Result<EvidencePack>::failure(error);
        }
        auto decoded = rag::decode_query_result(
            response.value(), request_id, retrieval_revision_, config_.top_k,
            config_.max_total_bytes);
        if (!decoded.has_value()) {
            break_process();
            return decoded;
        }
        state_ = State::Ready;
        return decoded;
    } catch (...) {
        break_process();
        return Result<EvidencePack>::failure(invalid_config());
    }
}

}  // namespace agent

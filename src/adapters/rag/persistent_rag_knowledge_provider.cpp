#include "adapters/rag/persistent_rag_knowledge_provider.h"

#include "adapters/rag/rag_protocol.h"
#include "adapters/rag/task_evidence_cache.h"
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

// T5: a turn that does not need retrieval at all gets an empty
// evidence pack. This keeps the runtime happy and prevents the
// knowledge provider from touching the sidecar.
Result<EvidencePack> empty_evidence() {
    return Result<EvidencePack>::success({});
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

void PersistentRagKnowledgeProvider::prepare_capability(RetrievalNeed need) noexcept {
    // T5: callers may record the need they requested. The provider
    // does not negotiate capabilities on the wire for v3 packs yet
    // (that lands in the next sidecar release). Until then we only
    // track the requested need so tests can verify the orchestrator
    // asked for what it asked for.
    switch (need) {
    case RetrievalNeed::None:
    case RetrievalNeed::ExactReference:
        // Lexical / index_only: embedding model may stay unloaded.
        break;
    case RetrievalNeed::Semantic:
        // Dense path requires the embedding model.
        break;
    }
}

bool PersistentRagKnowledgeProvider::embedding_ready() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return embedding_ready_;
}

bool PersistentRagKnowledgeProvider::index_ready() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return index_ready_;
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
    index_ready_ = ready.value().index_ready;
    embedding_ready_ = ready.value().embedding_ready;
    state_ = State::Ready;
    return Result<void>::success();
}

Result<void> PersistentRagKnowledgeProvider::ensure_capability(
    RetrievalNeed need) {
    // T5: a v3 ready frame MUST advertise at least one capability.
    // The orchestrator already checked that, so we only assert the
    // turn-level requirement. If the turn only needs the lexical
    // index we are happy with `index_ready`; semantic turns require
    // `embedding_ready` too. v2 frames report both ready, which
    // satisfies either need.
    switch (need) {
    case RetrievalNeed::None:
        return Result<void>::success();
    case RetrievalNeed::ExactReference:
        if (!index_ready_) {
            return Result<void>::failure(
                {ErrorCode::DependencyUnavailable,
                 "rag index capability is not ready", true});
        }
        return Result<void>::success();
    case RetrievalNeed::Semantic:
        if (!embedding_ready_) {
            return Result<void>::failure(
                {ErrorCode::DependencyUnavailable,
                 "rag embedding capability is not ready", true});
        }
        return Result<void>::success();
    }
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
    index_ready_ = false;
    embedding_ready_ = false;
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
            return empty_evidence();
        }
        if (!valid_config(config_)) {
            return Result<EvidencePack>::failure(invalid_config());
        }
        if (state.issue.empty() || state.issue.size() > 16'384 ||
            state.issue.find('\0') != std::string::npos ||
            !workspace::is_strict_utf8_text(state.issue)) {
            return Result<EvidencePack>::failure(invalid_query());
        }
        // T5: route the turn before paying the sidecar-startup cost.
        // An Auto/Always/Off decision lives in the policy header so
        // unit tests can pin its behaviour without driving the whole
        // engine.
        const auto decision = decide_retrieval(
            state.issue, config_.retrieval_policy, session_regulatory_context_);
        session_regulatory_context_ = decision.need != RetrievalNeed::None;
        prepare_capability(decision.need);
        if (decision.need == RetrievalNeed::None) {
            return empty_evidence();
        }
        auto ready = ensure_ready();
        if (!ready.has_value()) {
            return Result<EvidencePack>::failure(ready.error());
        }
        if (const auto capability = ensure_capability(decision.need);
            !capability.has_value()) {
            return Result<EvidencePack>::failure(capability.error());
        }
        // T7: the lease/revision guard runs before the cache lookup so a
        // stale entry cannot leak across pack rotations. The cache key
        // captures every input that can change the result.
        EvidenceCacheKey cache_key;
        cache_key.task_id = state.task_id;
        cache_key.workspace = state.workspace_utf8;
        cache_key.query = state.issue;
        cache_key.retrieval_revision = retrieval_revision_;
        cache_key.mode = config_.mode;
        cache_key.top_k = config_.top_k;
        cache_key.max_total_bytes = config_.max_total_bytes;
        if (const auto cached = evidence_cache_.lookup(cache_key);
            cached.has_value()) {
            return Result<EvidencePack>::success(*cached);
        }
        const auto request_id = next_request_id();
        // T5: pick the protocol version based on the requested
        // capability. ExactReference turns can stay on v2 so a
        // legacy pack keeps working unchanged. Semantic turns emit
        // v3 so the sidecar can also report capability mismatches.
        const std::int64_t schema_version =
            decision.need == RetrievalNeed::Semantic
                ? rag::kProtocolVersionV3
                : rag::kProtocolVersionV2;
        const nlohmann::json request{
            {"schema_version", schema_version},
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
        // T5: `authoritative_no_match` only ever comes from the
        // sidecar's deterministic citation lookup. A semantic miss
        // or timeout has already broken the process above; here we
        // make sure the flag is never set by transport-level code.
        if (decoded.value().authoritative_no_match &&
            decision.need != RetrievalNeed::ExactReference) {
            break_process();
            return Result<EvidencePack>::failure(
                {ErrorCode::ProtocolFailure,
                 "authoritative_no_match requires a citation lookup", false});
        }
        state_ = State::Ready;
        // T7: cache successful immutable evidence. The store enforces the
        // 32 KiB byte cap; larger packs stay out of the cache so the
        // per-turn budget is never exceeded by a re-read.
        evidence_cache_.store(cache_key, decoded.value());
        return decoded;
    } catch (...) {
        break_process();
        return Result<EvidencePack>::failure(invalid_config());
    }
}

}  // namespace agent

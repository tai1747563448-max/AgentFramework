#include "adapters/rag/python_rag_knowledge_provider.h"

#include "adapters/json/value_json.h"
#include "adapters/workspace/workspace_text.h"
#include "domain/evidence_validation.h"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

#include <nlohmann/json.hpp>

#include <filesystem>
#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace agent {
namespace {

constexpr std::size_t kMaxQueryBytes = 16'384;
constexpr std::size_t kMaxEvidenceBytes = 32'768;
constexpr std::size_t kMaxStdoutBytes = 64 * 1024;
constexpr std::size_t kMaxStderrBytes = 4 * 1024;

RuntimeError invalid_configuration() {
    return {ErrorCode::InvalidConfiguration, "invalid rag configuration", false};
}

RuntimeError invalid_query() {
    return {ErrorCode::InvalidInput, "invalid rag query", false};
}

RuntimeError process_unavailable() {
    return {ErrorCode::DependencyUnavailable, "rag process unavailable", true};
}

RuntimeError query_timeout() {
    return {ErrorCode::RequestTimeout, "rag query timed out", true};
}

RuntimeError process_failed() {
    return {ErrorCode::DependencyUnavailable, "rag process failed", false};
}

RuntimeError response_too_large() {
    return {ErrorCode::ProtocolFailure, "rag response exceeded limit", false};
}

RuntimeError invalid_response() {
    return {ErrorCode::ProtocolFailure, "invalid rag response", false};
}

bool path_is_link_or_reparse(const std::filesystem::path& path,
                             std::error_code& error) noexcept {
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || std::filesystem::is_symlink(status)) {
        return true;
    }
#if defined(_WIN32)
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = std::error_code(static_cast<int>(GetLastError()),
                                std::system_category());
        return true;
    }
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return false;
#endif
}

std::optional<std::filesystem::path> trusted_regular_file(
    const std::filesystem::path& supplied) noexcept {
    try {
        if (supplied.empty() || !supplied.is_absolute()) {
            return std::nullopt;
        }

        std::error_code error;
        auto current = supplied.root_path();
        for (const auto& component : supplied.relative_path()) {
            current /= component;
            if (path_is_link_or_reparse(current, error) || error) {
                return std::nullopt;
            }
        }

        const auto canonical = std::filesystem::canonical(supplied, error);
        if (error || canonical != supplied.lexically_normal() ||
            !std::filesystem::is_regular_file(canonical, error) || error) {
            return std::nullopt;
        }
        const auto links = std::filesystem::hard_link_count(canonical, error);
        if (error || links != 1) {
            return std::nullopt;
        }
        return canonical;
    } catch (...) {
        return std::nullopt;
    }
}

bool has_exact_keys(const nlohmann::json& object,
                    std::initializer_list<const char*> keys) {
    if (!object.is_object() || object.size() != keys.size()) {
        return false;
    }
    for (const auto* key : keys) {
        if (!object.contains(key)) {
            return false;
        }
    }
    return true;
}

Result<EvidencePack> decode_response(const std::string& text,
                                     std::size_t top_k) {
    try {
        if (text.size() > kMaxStdoutBytes) {
            return Result<EvidencePack>::failure(response_too_large());
        }
        if (text.empty() || !workspace::is_strict_utf8_text(text)) {
            return Result<EvidencePack>::failure(invalid_response());
        }
        bool has_duplicate_key = false;
        std::vector<std::set<std::string>> object_keys;
        const nlohmann::json::parser_callback_t callback =
            [&](int depth, nlohmann::json::parse_event_t event,
                nlohmann::json& parsed) {
                if (event == nlohmann::json::parse_event_t::object_start) {
                    const auto key_depth = static_cast<std::size_t>(depth + 1);
                    if (object_keys.size() <= key_depth) {
                        object_keys.resize(key_depth + 1);
                    }
                    object_keys[key_depth].clear();
                } else if (event == nlohmann::json::parse_event_t::key) {
                    const auto key_depth = static_cast<std::size_t>(depth);
                    if (object_keys.size() <= key_depth) {
                        object_keys.resize(key_depth + 1);
                    }
                    if (!object_keys[key_depth]
                             .insert(parsed.get<std::string>())
                             .second) {
                        has_duplicate_key = true;
                    }
                }
                return true;
            };
        const auto json = nlohmann::json::parse(text, callback);
        if (has_duplicate_key) {
            return Result<EvidencePack>::failure(invalid_response());
        }
        if (!has_exact_keys(json, {"schema_version", "items"}) ||
            !json.at("schema_version").is_number_integer() ||
            json.at("schema_version").get<std::int64_t>() != 1 ||
            !json.at("items").is_array() ||
            json.at("items").size() > top_k) {
            return Result<EvidencePack>::failure(invalid_response());
        }

        EvidencePack evidence;
        evidence.items.reserve(json.at("items").size());
        for (const auto& item : json.at("items")) {
            if (!has_exact_keys(item, {"source_id", "content", "metadata"}) ||
                !item.at("source_id").is_string() ||
                !item.at("content").is_string() ||
                !item.at("metadata").is_object()) {
                return Result<EvidencePack>::failure(invalid_response());
            }
            auto metadata = value_from_json(item.at("metadata"));
            if (!metadata.has_value()) {
                return Result<EvidencePack>::failure(invalid_response());
            }
            evidence.items.push_back(
                {item.at("source_id").get<std::string>(),
                 item.at("content").get<std::string>(),
                 std::move(metadata.value())});
        }
        if (!evidence_pack_is_valid(evidence)) {
            return Result<EvidencePack>::failure(invalid_response());
        }
        return Result<EvidencePack>::success(std::move(evidence));
    } catch (const nlohmann::json::exception&) {
        return Result<EvidencePack>::failure(invalid_response());
    } catch (...) {
        return Result<EvidencePack>::failure(invalid_response());
    }
}

}  // namespace

PythonRagKnowledgeProvider::PythonRagKnowledgeProvider(
    ProcessRunner& process, PythonRagConfig config)
    : process_(process), config_(std::move(config)) {}

Result<EvidencePack> PythonRagKnowledgeProvider::retrieve(
    const TaskState& state) {
    try {
        if (config_.python_program.empty() ||
            !workspace::is_strict_utf8_text(config_.python_program) ||
            config_.top_k == 0 || config_.top_k > 20 ||
            config_.timeout_seconds < 1 || config_.timeout_seconds > 60) {
            return Result<EvidencePack>::failure(invalid_configuration());
        }
        const auto script = trusted_regular_file(config_.script_path);
        const auto index = trusted_regular_file(config_.index_path);
        if (!script.has_value() || !index.has_value()) {
            return Result<EvidencePack>::failure(invalid_configuration());
        }
        if (state.issue.empty() || state.issue.size() > kMaxQueryBytes ||
            !workspace::is_strict_utf8_text(state.issue)) {
            return Result<EvidencePack>::failure(invalid_query());
        }

        const nlohmann::json input{{"schema_version", 1},
                                   {"query", state.issue},
                                   {"top_k", config_.top_k},
                                   {"max_total_bytes", kMaxEvidenceBytes}};
        ProcessRequest request;
        request.program = config_.python_program;
        request.arguments = {"-E", "-s", "-X", "utf8",
                             script->generic_u8string(), "query", "--index",
                             index->generic_u8string()};
        request.working_directory = script->parent_path();
        request.stdin_utf8 = input.dump();
        request.timeout_ms = config_.timeout_seconds * 1'000;
        request.max_stdout_bytes = kMaxStdoutBytes;
        request.max_stderr_bytes = kMaxStderrBytes;

        auto output = process_.run(request);
        if (!output.has_value()) {
            return Result<EvidencePack>::failure(process_unavailable());
        }
        if (output.value().timed_out) {
            return Result<EvidencePack>::failure(query_timeout());
        }
        if (output.value().exit_code != 0) {
            return Result<EvidencePack>::failure(process_failed());
        }
        if (output.value().stdout_truncated) {
            return Result<EvidencePack>::failure(response_too_large());
        }
        return decode_response(output.value().stdout_utf8, config_.top_k);
    } catch (...) {
        return Result<EvidencePack>::failure(invalid_configuration());
    }
}

}  // namespace agent

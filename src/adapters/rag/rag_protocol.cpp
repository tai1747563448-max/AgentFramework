#include "adapters/rag/rag_protocol.h"

#include "adapters/workspace/workspace_text.h"
#include "domain/evidence_validation.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace agent::rag {
namespace {

constexpr std::size_t kMaxLineBytes = 65'536;
constexpr std::int64_t kExpectedDocuments = 30'000;
constexpr std::int64_t kExpectedDimensions = 1'024;
constexpr const char* kServerRequestId =
    "req-00000000000000000000000000000000";

RuntimeError invalid_response() {
    return {ErrorCode::ProtocolFailure, "invalid rag protocol response", false};
}

template <typename T>
Result<T> failure() {
    return Result<T>::failure(invalid_response());
}

bool exact_keys(const nlohmann::json& object,
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

// exact_keys widened with a set of tolerated optional keys. Used by the
// schema-3 frames, where `backend` / `precision` may be present or absent
// depending on whether the index builder recorded embedding identity. The
// frame must still name every required key and nothing outside
// required ∪ optional, so an unknown key is still a protocol failure.
bool keys_within(const nlohmann::json& object,
                 std::initializer_list<const char*> required,
                 std::initializer_list<const char*> optional) {
    if (!object.is_object()) {
        return false;
    }
    for (const auto* key : required) {
        if (!object.contains(key)) {
            return false;
        }
    }
    for (const auto& entry : object.items()) {
        bool known = false;
        for (const auto* key : required) {
            known = known || entry.key() == key;
        }
        for (const auto* key : optional) {
            known = known || entry.key() == key;
        }
        if (!known) {
            return false;
        }
    }
    return true;
}

bool lower_hex(const std::string& text, std::size_t size) {
    if (text.size() != size) {
        return false;
    }
    for (const char value : text) {
        if (!((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool request_id_is_valid(const std::string& value) {
    return value.size() == 36 && value.compare(0, 4, "req-") == 0 &&
           lower_hex(value.substr(4), 32);
}

bool pack_id_is_valid(const std::string& value) {
    return value.size() == 37 && value.compare(0, 5, "pack-") == 0 &&
           lower_hex(value.substr(5), 32);
}

bool leap_year(int year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

bool canonical_date(const std::string& value) {
    if (value.size() != 10 || value[4] != '-' || value[7] != '-') {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index != 4 && index != 7 &&
            (value[index] < '0' || value[index] > '9')) {
            return false;
        }
    }
    const int year = std::stoi(value.substr(0, 4));
    const int month = std::stoi(value.substr(5, 2));
    const int day = std::stoi(value.substr(8, 2));
    if (year < 2000 || month < 1 || month > 12 || day < 1) {
        return false;
    }
    static constexpr int days[] = {31, 28, 31, 30, 31, 30,
                                   31, 31, 30, 31, 30, 31};
    const int maximum = month == 2 && leap_year(year)
                            ? 29
                            : days[month - 1];
    return day <= maximum;
}

bool bounded_text(const std::string& value, std::size_t maximum,
                  bool allow_newlines = false) {
    if (value.empty() || value.size() > maximum ||
        value.find('\0') != std::string::npos ||
        !workspace::is_strict_utf8_text(value)) {
        return false;
    }
    if (allow_newlines) {
        return true;
    }
    for (const unsigned char byte : value) {
        if (byte < 0x20U || byte == 0x7FU) {
            return false;
        }
    }
    return true;
}

bool relative_corpus_markdown_path(const std::string& value) {
    if (!bounded_text(value, 512) || value.compare(0, 7, "corpus/") != 0 ||
        value.size() < 10 || value.substr(value.size() - 3) != ".md" ||
        value.find('\\') != std::string::npos ||
        value.find(':') != std::string::npos || value.front() == '/') {
        return false;
    }
    std::size_t start = 0;
    while (start < value.size()) {
        const auto end = value.find('/', start);
        const auto length = (end == std::string::npos ? value.size() : end) - start;
        if (length == 0 || (length == 1 && value[start] == '.') ||
            (length == 2 && value[start] == '.' && value[start + 1] == '.')) {
            return false;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return true;
}

bool official_url_is_valid(const std::string& value,
                           const std::string& snapshot_date) {
    constexpr const char* prefix = "https://www.ecfr.gov/";
    if (!bounded_text(value, 2'048) || value.compare(0, 21, prefix) != 0 ||
        value.find('@') != std::string::npos ||
        value.find('#') != std::string::npos) {
        return false;
    }
    return value.find("/on/" + snapshot_date + "/") == 20;
}

bool source_id_is_valid(const std::string& value) {
    constexpr std::size_t document_prefix = 4;
    constexpr std::size_t document_hex = 32;
    constexpr std::size_t chunk_prefix = 7;
    constexpr std::size_t chunk_hex = 16;
    if (value.size() != document_prefix + document_hex + chunk_prefix + chunk_hex ||
        value.compare(0, document_prefix, "doc-") != 0 ||
        value.compare(document_prefix + document_hex, chunk_prefix, "-chunk-") != 0) {
        return false;
    }
    return lower_hex(value.substr(document_prefix, document_hex), document_hex) &&
           lower_hex(value.substr(document_prefix + document_hex + chunk_prefix),
                     chunk_hex);
}

bool parse_json(const std::string& line, nlohmann::json& decoded) {
    if (line.empty() || line.size() > kMaxLineBytes ||
        !workspace::is_strict_utf8_text(line)) {
        return false;
    }
    for (const unsigned char byte : line) {
        if (byte < 0x20U || byte == 0x7FU) {
            return false;
        }
    }
    bool duplicate = false;
    std::vector<std::set<std::string>> keys;
    const nlohmann::json::parser_callback_t callback =
        [&](int depth, nlohmann::json::parse_event_t event,
            nlohmann::json& parsed) {
            if (event == nlohmann::json::parse_event_t::object_start) {
                const auto index = static_cast<std::size_t>(depth + 1);
                if (keys.size() <= index) {
                    keys.resize(index + 1);
                }
                keys[index].clear();
            } else if (event == nlohmann::json::parse_event_t::key) {
                const auto index = static_cast<std::size_t>(depth);
                if (keys.size() <= index) {
                    keys.resize(index + 1);
                }
                if (!keys[index].insert(parsed.get<std::string>()).second) {
                    duplicate = true;
                }
            }
            return true;
        };
    try {
        decoded = nlohmann::json::parse(line, callback, true, false);
        return !decoded.is_discarded() && !duplicate;
    } catch (...) {
        return false;
    }
}

// T5: a ready-frame envelope that requires a specific schema version.
// Each decoder rejects every other version, which is what the plan
// asks for: "v3 decoder must not consume v2 frames (and vice versa)".
bool exact_envelope_version(const nlohmann::json& value, const char* op,
                            const std::string& request_id,
                            std::int64_t schema_version) {
    return exact_keys(value, {"schema_version", "request_id", "op", "payload"}) &&
           value.at("schema_version").is_number_integer() &&
           value.at("schema_version").get<std::int64_t>() == schema_version &&
           value.at("request_id").is_string() &&
           value.at("request_id").get_ref<const std::string&>() == request_id &&
           request_id_is_valid(request_id) && value.at("op").is_string() &&
           value.at("op").get_ref<const std::string&>() == op &&
           value.at("payload").is_object();
}

bool exact_envelope(const nlohmann::json& value, const char* op,
                    const std::string& request_id,
                    std::int64_t schema_version) {
    return exact_envelope_version(value, op, request_id, schema_version);
}

bool positive_rank_or_null(const nlohmann::json& value) {
    if (value.is_null()) {
        return true;
    }
    return value.is_number_integer() && value.get<std::int64_t>() >= 1 &&
           value.get<std::int64_t>() <= 100;
}

Value metadata_value(const nlohmann::json& metadata) {
    Value::Object result;
    for (const auto& entry : metadata.items()) {
        const auto& value = entry.value();
        if (value.is_null()) {
            result.emplace(entry.key(), Value());
        } else if (value.is_boolean()) {
            result.emplace(entry.key(), Value(value.get<bool>()));
        } else if (value.is_number_integer()) {
            result.emplace(entry.key(), Value(value.get<std::int64_t>()));
        } else if (value.is_number()) {
            result.emplace(entry.key(), Value(value.get<double>()));
        } else {
            result.emplace(entry.key(), Value(value.get<std::string>()));
        }
    }
    return Value::object(std::move(result));
}

bool valid_item(const nlohmann::json& item) {
    if (!exact_keys(item, {"source_id", "content", "metadata"}) ||
        !item.at("source_id").is_string() || !item.at("content").is_string() ||
        !item.at("metadata").is_object()) {
        return false;
    }
    const auto& source_id = item.at("source_id").get_ref<const std::string&>();
    const auto& content = item.at("content").get_ref<const std::string&>();
    const auto& metadata = item.at("metadata");
    if (!source_id_is_valid(source_id) || !bounded_text(content, 8'192, true) ||
        !exact_keys(metadata,
                    {"citation", "path", "start_line", "end_line",
                     "snapshot_date", "official_url", "content_sha256",
                     "document_sha256", "retrieval_revision", "bm25_rank", "dense_rank",
                     "fusion_score", "is_neighbor", "neighbor_of"})) {
        return false;
    }
    for (const auto* name : {"citation", "path", "snapshot_date", "official_url",
                             "content_sha256", "document_sha256",
                             "retrieval_revision"}) {
        if (!metadata.at(name).is_string()) {
            return false;
        }
    }
    if (!metadata.at("start_line").is_number_integer() ||
        !metadata.at("end_line").is_number_integer() ||
        !metadata.at("fusion_score").is_number() ||
        !metadata.at("is_neighbor").is_boolean() ||
        !positive_rank_or_null(metadata.at("bm25_rank")) ||
        !positive_rank_or_null(metadata.at("dense_rank")) ||
        !(metadata.at("neighbor_of").is_null() ||
          metadata.at("neighbor_of").is_string())) {
        return false;
    }
    const auto& citation = metadata.at("citation").get_ref<const std::string&>();
    const auto& path = metadata.at("path").get_ref<const std::string&>();
    const auto& date = metadata.at("snapshot_date").get_ref<const std::string&>();
    const auto& url = metadata.at("official_url").get_ref<const std::string&>();
    const auto& content_sha =
        metadata.at("content_sha256").get_ref<const std::string&>();
    const auto& document_sha =
        metadata.at("document_sha256").get_ref<const std::string&>();
    const auto& retrieval_revision =
        metadata.at("retrieval_revision").get_ref<const std::string&>();
    const auto start = metadata.at("start_line").get<std::int64_t>();
    const auto end = metadata.at("end_line").get<std::int64_t>();
    const auto score = metadata.at("fusion_score").get<double>();
    const bool neighbor = metadata.at("is_neighbor").get<bool>();
    const bool neighbor_id = metadata.at("neighbor_of").is_string();
    const bool has_rank = !metadata.at("bm25_rank").is_null() ||
                          !metadata.at("dense_rank").is_null();
    if (!bounded_text(citation, 256) || !relative_corpus_markdown_path(path) ||
        !canonical_date(date) || !official_url_is_valid(url, date) ||
        !lower_hex(content_sha, 64) ||
        content_sha != workspace::sha256_hex(content) ||
        !lower_hex(document_sha, 64) ||
        retrieval_revision.size() != 74U ||
        retrieval_revision.rfind("retrieval-", 0) != 0 ||
        !lower_hex(retrieval_revision.substr(10), 64) ||
        start < 1 || end < start ||
        end > 10'000'000 || !std::isfinite(score) || score < 0.0 ||
        (neighbor && (has_rank || score != 0.0 || !neighbor_id)) ||
        (!neighbor && (!has_rank || score <= 0.0 || neighbor_id))) {
        return false;
    }
    return !neighbor_id || source_id_is_valid(
                               metadata.at("neighbor_of").get_ref<const std::string&>());
}

bool validate_core_ready_fields(const ReadyInfo& ready) {
    return pack_id_is_valid(ready.pack_id) &&
           ready.retrieval_revision.size() == 74U &&
           ready.retrieval_revision.rfind("retrieval-", 0) == 0 &&
           lower_hex(ready.retrieval_revision.substr(10), 64) &&
           canonical_date(ready.snapshot_date) &&
           ready.document_count == kExpectedDocuments &&
           ready.chunk_count >= ready.document_count &&
           ready.model == "BAAI/bge-m3" && lower_hex(ready.revision, 40) &&
           ready.dimensions == kExpectedDimensions &&
           (ready.device == "cpu" || ready.device == "cuda");
}

}  // namespace

Result<ReadyInfo> decode_v2_ready(const std::string& line) {
    try {
        nlohmann::json value;
        if (!parse_json(line, value) ||
            !exact_envelope(value, "ready", kServerRequestId,
                            kProtocolVersionV2)) {
            return failure<ReadyInfo>();
        }
        const auto& payload = value.at("payload");
        if (!exact_keys(payload,
                        {"pack_id", "snapshot_date", "document_count",
                         "retrieval_revision", "chunk_count", "model", "revision", "dimensions",
                         "device"}) ||
            !payload.at("pack_id").is_string() ||
            !payload.at("retrieval_revision").is_string() ||
            !payload.at("snapshot_date").is_string() ||
            !payload.at("document_count").is_number_integer() ||
            !payload.at("chunk_count").is_number_integer() ||
            !payload.at("model").is_string() ||
            !payload.at("revision").is_string() ||
            !payload.at("dimensions").is_number_integer() ||
            !payload.at("device").is_string()) {
            return failure<ReadyInfo>();
        }
        ReadyInfo result{
            payload.at("pack_id").get<std::string>(),
            payload.at("retrieval_revision").get<std::string>(),
            payload.at("snapshot_date").get<std::string>(),
            payload.at("document_count").get<std::int64_t>(),
            payload.at("chunk_count").get<std::int64_t>(),
            payload.at("model").get<std::string>(),
            payload.at("revision").get<std::string>(),
            payload.at("dimensions").get<std::int64_t>(),
            payload.at("device").get<std::string>(),
            true,
            true};
        if (!validate_core_ready_fields(result)) {
            return failure<ReadyInfo>();
        }
        return Result<ReadyInfo>::success(std::move(result));
    } catch (...) {
        return failure<ReadyInfo>();
    }
}

Result<ReadyInfo> decode_v3_ready(const std::string& line) {
    try {
        nlohmann::json value;
        if (!parse_json(line, value) ||
            !exact_envelope(value, "ready", kServerRequestId,
                            kProtocolVersionV3)) {
            return failure<ReadyInfo>();
        }
        const auto& payload = value.at("payload");
        // T19 schema-3 identity: `backend` and `precision` are optional
        // extra keys. The index builder only writes them when the pack
        // carries embedding identity, so a pack without them is still
        // valid. Mirror hybrid_retriever.py: the pair is all-or-nothing
        // and each half must be a non-empty string, which is what makes
        // a half-updated pack detectable instead of silently accepted.
        const bool names_backend = payload.is_object() &&
                                   payload.contains("backend");
        const bool names_precision = payload.is_object() &&
                                     payload.contains("precision");
        const bool identity_is_valid =
            names_backend == names_precision &&
            (!names_backend ||
             (payload.at("backend").is_string() &&
              !payload.at("backend").get_ref<const std::string&>().empty() &&
              payload.at("precision").is_string() &&
              !payload.at("precision").get_ref<const std::string&>().empty()));
        if (!identity_is_valid ||
            !keys_within(payload,
                         {"pack_id", "snapshot_date", "document_count",
                          "retrieval_revision", "chunk_count", "model", "revision",
                          "dimensions", "device", "index_ready", "embedding_ready"},
                         {"backend", "precision"}) ||
            !payload.at("pack_id").is_string() ||
            !payload.at("retrieval_revision").is_string() ||
            !payload.at("snapshot_date").is_string() ||
            !payload.at("document_count").is_number_integer() ||
            !payload.at("chunk_count").is_number_integer() ||
            !payload.at("model").is_string() ||
            !payload.at("revision").is_string() ||
            !payload.at("dimensions").is_number_integer() ||
            !payload.at("device").is_string() ||
            !payload.at("index_ready").is_boolean() ||
            !payload.at("embedding_ready").is_boolean()) {
            return failure<ReadyInfo>();
        }
        ReadyInfo result{
            payload.at("pack_id").get<std::string>(),
            payload.at("retrieval_revision").get<std::string>(),
            payload.at("snapshot_date").get<std::string>(),
            payload.at("document_count").get<std::int64_t>(),
            payload.at("chunk_count").get<std::int64_t>(),
            payload.at("model").get<std::string>(),
            payload.at("revision").get<std::string>(),
            payload.at("dimensions").get<std::int64_t>(),
            payload.at("device").get<std::string>(),
            payload.at("index_ready").get<bool>(),
            payload.at("embedding_ready").get<bool>()};
        if (!validate_core_ready_fields(result)) {
            return failure<ReadyInfo>();
        }
        // T5: the v3 ready frame MUST assert at least one usable
        // capability. Decoding a frame that says "nothing is ready"
        // lets the orchestrator start a turn on a broken sidecar.
        if (!result.index_ready && !result.embedding_ready) {
            return failure<ReadyInfo>();
        }
        return Result<ReadyInfo>::success(std::move(result));
    } catch (...) {
        return failure<ReadyInfo>();
    }
}

Result<ReadyInfo> decode_ready(const std::string& line) {
    // T5: the unified entry point. It auto-detects the schema version
    // so a binary that ships with the v3 decoder can still talk to a
    // legacy pack, but a v3 ready frame sent to a v2-only client is
    // rejected because the decoder it routes to requires an exact
    // version match.
    try {
        nlohmann::json value;
        if (!parse_json(line, value) ||
            !value.is_object() || !value.contains("schema_version") ||
            !value.at("schema_version").is_number_integer()) {
            return failure<ReadyInfo>();
        }
        const auto version = value.at("schema_version").get<std::int64_t>();
        if (version == kProtocolVersionV2) return decode_v2_ready(line);
        if (version == kProtocolVersionV3) return decode_v3_ready(line);
        return failure<ReadyInfo>();
    } catch (...) {
        return failure<ReadyInfo>();
    }
}

Result<EvidencePack> decode_query_result(const std::string& line,
                                         const std::string& expected_request_id,
                                         const std::string& expected_retrieval_revision,
                                         std::size_t top_k,
                                         std::size_t max_total_bytes) {
    try {
        if (top_k == 0 || top_k > 20 || max_total_bytes == 0 ||
            max_total_bytes > 32'768 ||
            !request_id_is_valid(expected_request_id) ||
            expected_retrieval_revision.size() != 74U ||
            expected_retrieval_revision.rfind("retrieval-", 0) != 0 ||
            !lower_hex(expected_retrieval_revision.substr(10), 64)) {
            return failure<EvidencePack>();
        }
        nlohmann::json value;
        if (!parse_json(line, value) ||
            !value.is_object() || !value.contains("schema_version") ||
            !value.at("schema_version").is_number_integer()) {
            return failure<EvidencePack>();
        }
        const auto version = value.at("schema_version").get<std::int64_t>();
        // T5: query_result decoding accepts either protocol version so
        // the same orchestrator code can drive legacy and new packs.
        // The error path below uses the same version so the client
        // and server agree on the envelope.
        if (version != kProtocolVersionV2 && version != kProtocolVersionV3) {
            return failure<EvidencePack>();
        }
        if (!exact_envelope(value, "query_result", expected_request_id, version) ||
            !exact_keys(value.at("payload"), {"items", "outcome"}) ||
            !value.at("payload").at("items").is_array() ||
            !value.at("payload").at("outcome").is_string() ||
            value.at("payload").at("items").size() > top_k) {
            return failure<EvidencePack>();
        }
        EvidencePack result;
        const auto& outcome = value.at("payload")
                                  .at("outcome")
                                  .get_ref<const std::string&>();
        const bool has_items = !value.at("payload").at("items").empty();
        if ((outcome != "matched" && outcome != "no_match" &&
             outcome != "authoritative_no_match") ||
            (outcome == "matched") != has_items) {
            return failure<EvidencePack>();
        }
        // T5: `authoritative_no_match` may only come from a defined
        // authoritative mapping. Init failure, semantic miss, and
        // timeout are reported as transport / protocol failures and
        // never set this flag, so the runtime cannot present an
        // authoritative no-match message unless the sidecar proved
        // the citation was checked.
        result.authoritative_no_match = outcome == "authoritative_no_match";
        result.tool_use_forbidden = has_items;
        std::size_t total_bytes = 0;
        std::set<std::string> source_ids;
        std::set<std::string> content_shas;
        for (const auto& item : value.at("payload").at("items")) {
            if (!valid_item(item)) {
                return failure<EvidencePack>();
            }
            if (item.at("metadata").at("retrieval_revision") !=
                expected_retrieval_revision) {
                return failure<EvidencePack>();
            }
            const auto& content = item.at("content").get_ref<const std::string&>();
            if (content.size() > max_total_bytes - total_bytes) {
                return failure<EvidencePack>();
            }
            total_bytes += content.size();
            const auto& source_id =
                item.at("source_id").get_ref<const std::string&>();
            const auto& sha = item.at("metadata")
                                  .at("content_sha256")
                                  .get_ref<const std::string&>();
            if (!source_ids.insert(source_id).second ||
                !content_shas.insert(sha).second) {
                return failure<EvidencePack>();
            }
            result.items.push_back({source_id, content,
                                    metadata_value(item.at("metadata"))});
        }
        for (const auto& item : value.at("payload").at("items")) {
            const auto& metadata = item.at("metadata");
            if (metadata.at("is_neighbor").get<bool>()) {
                const auto& parent =
                    metadata.at("neighbor_of").get_ref<const std::string&>();
                if (source_ids.find(parent) == source_ids.end()) {
                    return failure<EvidencePack>();
                }
            }
        }
        if (!evidence_pack_is_valid(result)) {
            return failure<EvidencePack>();
        }
        return Result<EvidencePack>::success(std::move(result));
    } catch (...) {
        return failure<EvidencePack>();
    }
}

}  // namespace agent::rag

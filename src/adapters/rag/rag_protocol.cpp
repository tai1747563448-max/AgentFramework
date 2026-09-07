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

bool exact_envelope(const nlohmann::json& value, const char* op,
                    const std::string& request_id) {
    return exact_keys(value, {"schema_version", "request_id", "op", "payload"}) &&
           value.at("schema_version").is_number_integer() &&
           value.at("schema_version").get<std::int64_t>() == 2 &&
           value.at("request_id").is_string() &&
           value.at("request_id").get_ref<const std::string&>() == request_id &&
           request_id_is_valid(request_id) && value.at("op").is_string() &&
           value.at("op").get_ref<const std::string&>() == op &&
           value.at("payload").is_object();
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
                     "document_sha256", "bm25_rank", "dense_rank",
                     "fusion_score", "is_neighbor", "neighbor_of"})) {
        return false;
    }
    for (const auto* name : {"citation", "path", "snapshot_date", "official_url",
                             "content_sha256", "document_sha256"}) {
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
        !lower_hex(document_sha, 64) || start < 1 || end < start ||
        end > 10'000'000 || !std::isfinite(score) || score < 0.0 ||
        (neighbor && (has_rank || score != 0.0 || !neighbor_id)) ||
        (!neighbor && (!has_rank || score <= 0.0 || neighbor_id))) {
        return false;
    }
    return !neighbor_id || source_id_is_valid(
                               metadata.at("neighbor_of").get_ref<const std::string&>());
}

}  // namespace

Result<ReadyInfo> decode_ready(const std::string& line) {
    try {
        nlohmann::json value;
        if (!parse_json(line, value) ||
            !exact_envelope(value, "ready", kServerRequestId)) {
            return failure<ReadyInfo>();
        }
        const auto& payload = value.at("payload");
        if (!exact_keys(payload,
                        {"pack_id", "snapshot_date", "document_count",
                         "chunk_count", "model", "revision", "dimensions",
                         "device"}) ||
            !payload.at("pack_id").is_string() ||
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
            payload.at("snapshot_date").get<std::string>(),
            payload.at("document_count").get<std::int64_t>(),
            payload.at("chunk_count").get<std::int64_t>(),
            payload.at("model").get<std::string>(),
            payload.at("revision").get<std::string>(),
            payload.at("dimensions").get<std::int64_t>(),
            payload.at("device").get<std::string>()};
        if (!pack_id_is_valid(result.pack_id) ||
            !canonical_date(result.snapshot_date) ||
            result.document_count != kExpectedDocuments ||
            result.chunk_count < result.document_count ||
            result.model != "BAAI/bge-m3" || !lower_hex(result.revision, 40) ||
            result.dimensions != kExpectedDimensions ||
            (result.device != "cpu" && result.device != "cuda")) {
            return failure<ReadyInfo>();
        }
        return Result<ReadyInfo>::success(std::move(result));
    } catch (...) {
        return failure<ReadyInfo>();
    }
}

Result<EvidencePack> decode_query_result(const std::string& line,
                                         const std::string& expected_request_id,
                                         std::size_t top_k,
                                         std::size_t max_total_bytes) {
    try {
        if (top_k == 0 || top_k > 20 || max_total_bytes == 0 ||
            max_total_bytes > 32'768 ||
            !request_id_is_valid(expected_request_id)) {
            return failure<EvidencePack>();
        }
        nlohmann::json value;
        if (!parse_json(line, value) ||
            !exact_envelope(value, "query_result", expected_request_id) ||
            !exact_keys(value.at("payload"), {"items"}) ||
            !value.at("payload").at("items").is_array() ||
            value.at("payload").at("items").size() > top_k) {
            return failure<EvidencePack>();
        }
        EvidencePack result;
        std::size_t total_bytes = 0;
        std::set<std::string> source_ids;
        std::set<std::string> content_shas;
        for (const auto& item : value.at("payload").at("items")) {
            if (!valid_item(item)) {
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

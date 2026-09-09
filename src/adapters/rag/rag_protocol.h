#pragma once

#include "domain/model_types.h"
#include "domain/result.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace agent::rag {

struct ReadyInfo {
    std::string pack_id;
    std::string retrieval_revision;
    std::string snapshot_date;
    std::int64_t document_count{0};
    std::int64_t chunk_count{0};
    std::string model;
    std::string revision;
    std::int64_t dimensions{0};
    std::string device;
};

Result<ReadyInfo> decode_ready(const std::string& line);
Result<EvidencePack> decode_query_result(const std::string& line,
                                         const std::string& expected_request_id,
                                         const std::string& expected_retrieval_revision,
                                         std::size_t top_k,
                                         std::size_t max_total_bytes);

}  // namespace agent::rag

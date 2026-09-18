#pragma once

#include "domain/model_types.h"
#include "domain/result.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace agent::rag {

// T5: protocol v3 separates the sidecar capabilities so the client can
// request the minimum it needs for a turn. `index_ready` means the
// lexical/statistical path is open; `embedding_ready` means the dense
// BGE-M3 path is also live. A v3 client that only needs ExactReference
// can target `index_ready` and skip loading the embedding model.
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
    bool index_ready{false};
    bool embedding_ready{false};
};

// T5: protocol versions recognised by this build. v2 stays valid for
// legacy packs; v3 is the default for new packs. Decoders must not
// accept frames whose `schema_version` does not match the decoder they
// were written for.
constexpr std::int64_t kProtocolVersionV2 = 2;
constexpr std::int64_t kProtocolVersionV3 = 3;

// T5: required v3 ready capabilities. Either `index_ready` or
// `embedding_ready` must be true for the ready frame to be useful.
// Decoding both fields as false is rejected so the orchestrator never
// silently starts a turn on a partially-loaded sidecar.
Result<ReadyInfo> decode_ready(const std::string& line);
Result<ReadyInfo> decode_v2_ready(const std::string& line);
Result<ReadyInfo> decode_v3_ready(const std::string& line);
Result<EvidencePack> decode_query_result(const std::string& line,
                                         const std::string& expected_request_id,
                                         const std::string& expected_retrieval_revision,
                                         std::size_t top_k,
                                         std::size_t max_total_bytes);

}  // namespace agent::rag
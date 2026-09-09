#include "adapters/rag/rag_protocol.h"
#include "adapters/workspace/workspace_text.h"
#include "test_support.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fixtures {

constexpr const char* kRequestId =
    "req-00000000000000000000000000000001";
const std::string kRetrievalRevision = "retrieval-" + std::string(64, 'e');

nlohmann::json envelope(std::string op, nlohmann::json payload,
                        std::string request_id = kRequestId) {
    return {{"schema_version", 2},
            {"request_id", std::move(request_id)},
            {"op", std::move(op)},
            {"payload", std::move(payload)}};
}

nlohmann::json ready() {
    return envelope(
        "ready",
        {{"pack_id", "pack-cccccccccccccccccccccccccccccccc"},
         {"retrieval_revision", kRetrievalRevision},
         {"snapshot_date", "2026-09-03"},
         {"document_count", 30'000},
         {"chunk_count", 45'000},
         {"model", "BAAI/bge-m3"},
         {"revision", "dddddddddddddddddddddddddddddddddddddddd"},
         {"dimensions", 1024},
         {"device", "cpu"}},
        "req-00000000000000000000000000000000");
}

nlohmann::json evidence(std::string content = "Legal evidence") {
    const auto sha = agent::workspace::sha256_hex(content);
    return {
        {"source_id", "doc-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-chunk-0000000000000001"},
        {"content", std::move(content)},
        {"metadata",
         {{"citation", "40 CFR 60.1"},
          {"path", "corpus/title-040/section-60.1.md"},
          {"start_line", 10},
          {"end_line", 12},
          {"snapshot_date", "2026-09-03"},
          {"official_url",
           "https://www.ecfr.gov/on/2026-09-03/title-40/section-60.1"},
          {"content_sha256", sha},
          {"document_sha256", std::string(64, 'b')},
          {"retrieval_revision", kRetrievalRevision},
          {"bm25_rank", 1},
          {"dense_rank", 2},
          {"fusion_score", 0.03},
          {"is_neighbor", false},
          {"neighbor_of", nullptr}}}};
}

nlohmann::json query(nlohmann::json item = evidence()) {
    return envelope("query_result",
                    {{"outcome", "matched"},
                     {"items", nlohmann::json::array({item})}});
}

void require_protocol_failure(const agent::Result<agent::EvidencePack>& result) {
    REQUIRE(!result.has_value());
    REQUIRE(result.error().code == agent::ErrorCode::ProtocolFailure);
    REQUIRE(result.error().message == "invalid rag protocol response");
    REQUIRE(!result.error().retryable);
}

}  // namespace fixtures

TEST_CASE(rag_protocol_decodes_exact_ready_and_query_result) {
    const auto ready = agent::rag::decode_ready(fixtures::ready().dump());
    REQUIRE(ready.has_value());
    REQUIRE(ready.value().pack_id ==
            "pack-cccccccccccccccccccccccccccccccc");
    REQUIRE(ready.value().document_count == 30'000);
    REQUIRE(ready.value().dimensions == 1024);

    const auto decoded = agent::rag::decode_query_result(
        fixtures::query().dump(), fixtures::kRequestId,
        fixtures::kRetrievalRevision, 6, 32'768);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded.value().items.size() == 1);
    const auto& item = decoded.value().items.front();
    REQUIRE(item.metadata.at("citation").as_string() == "40 CFR 60.1");
    REQUIRE(item.metadata.at("snapshot_date").as_string() == "2026-09-03");
    REQUIRE(item.metadata.at("bm25_rank").as_integer() == 1);
    REQUIRE(item.metadata.at("dense_rank").as_integer() == 2);
    REQUIRE(item.metadata.at("fusion_score").as_double() == 0.03);
    REQUIRE(!decoded.value().authoritative_no_match);
    REQUIRE(decoded.value().tool_use_forbidden);
}

TEST_CASE(rag_protocol_decodes_authoritative_no_match_without_evidence) {
    const auto response = fixtures::envelope(
        "query_result",
        {{"outcome", "authoritative_no_match"},
         {"items", nlohmann::json::array()}});

    const auto decoded = agent::rag::decode_query_result(
        response.dump(), fixtures::kRequestId,
        fixtures::kRetrievalRevision, 6, 32'768);

    REQUIRE(decoded.has_value());
    REQUIRE(decoded.value().items.empty());
    REQUIRE(decoded.value().authoritative_no_match);
    REQUIRE(!decoded.value().tool_use_forbidden);
}

TEST_CASE(rag_protocol_rejects_ready_schema_and_identity_mutations) {
    std::vector<nlohmann::json> invalid;
    auto extra = fixtures::ready();
    extra["extra"] = true;
    invalid.push_back(std::move(extra));
    auto count = fixtures::ready();
    count["payload"]["document_count"] = 29'999;
    invalid.push_back(std::move(count));
    auto dimensions = fixtures::ready();
    dimensions["payload"]["dimensions"] = 384;
    invalid.push_back(std::move(dimensions));
    auto model = fixtures::ready();
    model["payload"]["model"] = "other";
    invalid.push_back(std::move(model));
    auto request = fixtures::ready();
    request["request_id"] = fixtures::kRequestId;
    invalid.push_back(std::move(request));

    for (const auto& response : invalid) {
        const auto decoded = agent::rag::decode_ready(response.dump());
        REQUIRE(!decoded.has_value());
        REQUIRE(decoded.error().code == agent::ErrorCode::ProtocolFailure);
        REQUIRE(decoded.error().message == "invalid rag protocol response");
    }
}

TEST_CASE(rag_protocol_rejects_forged_or_unbounded_evidence) {
    std::vector<nlohmann::json> invalid;
    auto wrong_id = fixtures::query();
    wrong_id["request_id"] =
        "req-00000000000000000000000000000002";
    invalid.push_back(std::move(wrong_id));
    auto extra = fixtures::query();
    extra["payload"]["items"][0]["metadata"]["extra"] = true;
    invalid.push_back(std::move(extra));
    auto digest = fixtures::query();
    digest["payload"]["items"][0]["metadata"]["content_sha256"] =
        std::string(64, '0');
    invalid.push_back(std::move(digest));
    auto path = fixtures::query();
    path["payload"]["items"][0]["metadata"]["path"] = "../private.md";
    invalid.push_back(std::move(path));
    auto url = fixtures::query();
    url["payload"]["items"][0]["metadata"]["official_url"] =
        "https://example.com/forged";
    invalid.push_back(std::move(url));
    auto rank = fixtures::query();
    rank["payload"]["items"][0]["metadata"]["dense_rank"] = true;
    invalid.push_back(std::move(rank));
    auto retrieval_revision = fixtures::query();
    retrieval_revision["payload"]["items"][0]["metadata"]
                      ["retrieval_revision"] =
        "retrieval-" + std::string(64, 'f');
    invalid.push_back(std::move(retrieval_revision));
    auto empty_matched = fixtures::query();
    empty_matched["payload"]["items"] = nlohmann::json::array();
    invalid.push_back(std::move(empty_matched));
    auto no_match_with_item = fixtures::query();
    no_match_with_item["payload"]["outcome"] = "authoritative_no_match";
    invalid.push_back(std::move(no_match_with_item));
    auto unknown_outcome = fixtures::query();
    unknown_outcome["payload"]["outcome"] = "maybe";
    invalid.push_back(std::move(unknown_outcome));

    for (const auto& response : invalid) {
        fixtures::require_protocol_failure(agent::rag::decode_query_result(
            response.dump(), fixtures::kRequestId,
            fixtures::kRetrievalRevision, 6, 32'768));
    }

    auto too_many = fixtures::query();
    too_many["payload"]["items"].push_back(fixtures::evidence("second"));
    fixtures::require_protocol_failure(agent::rag::decode_query_result(
        too_many.dump(), fixtures::kRequestId,
        fixtures::kRetrievalRevision, 1, 32'768));

    auto too_large = fixtures::query(fixtures::evidence(std::string(101, 'x')));
    fixtures::require_protocol_failure(agent::rag::decode_query_result(
        too_large.dump(), fixtures::kRequestId,
        fixtures::kRetrievalRevision, 6, 100));
}

TEST_CASE(rag_protocol_rejects_duplicate_keys_and_nonfinite_numbers) {
    const std::string duplicate =
        R"({"schema_version":2,"schema_version":2,"request_id":"req-00000000000000000000000000000001","op":"query_result","payload":{"outcome":"no_match","items":[]}})";
    fixtures::require_protocol_failure(agent::rag::decode_query_result(
        duplicate, fixtures::kRequestId,
        fixtures::kRetrievalRevision, 6, 32'768));
    const std::string nonfinite =
        R"({"schema_version":2,"request_id":"req-00000000000000000000000000000001","op":"query_result","payload":{"outcome":"matched","items":[{"source_id":"doc-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-chunk-0000000000000001","content":"x","metadata":{"citation":"40 CFR 60.1","path":"corpus/title-040/section-60.1.md","start_line":1,"end_line":1,"snapshot_date":"2026-09-03","official_url":"https://www.ecfr.gov/on/2026-09-03/title-40/section-60.1","content_sha256":"2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881","document_sha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","retrieval_revision":"retrieval-eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee","bm25_rank":1,"dense_rank":null,"fusion_score":NaN,"is_neighbor":false,"neighbor_of":null}}]}})";
    fixtures::require_protocol_failure(agent::rag::decode_query_result(
        nonfinite, fixtures::kRequestId,
        fixtures::kRetrievalRevision, 6, 32'768));
}

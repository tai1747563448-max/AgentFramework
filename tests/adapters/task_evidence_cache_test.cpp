#include "adapters/rag/task_evidence_cache.h"

#include "test_support.h"

#include <string>

namespace {

agent::EvidenceCacheKey make_key(const std::string& query = "14 CFR 25.1309") {
    agent::EvidenceCacheKey key;
    key.task_id = "task-001";
    key.workspace = "ws-1";
    key.query = query;
    key.retrieval_revision = "rev-A";
    key.mode = "hybrid";
    key.top_k = 6;
    key.max_total_bytes = 32'768;
    return key;
}

agent::EvidencePack make_pack(const std::string& text) {
    agent::EvidencePack pack;
    agent::Evidence item;
    item.source_id = "14-CFR-25.1309";
    item.content = text;
    pack.items.push_back(std::move(item));
    return pack;
}

}  // namespace

TEST_CASE(TaskEvidenceCacheMissBeforeStore) {
    agent::TaskEvidenceCache cache;
    REQUIRE(!cache.lookup(make_key()).has_value());
}

TEST_CASE(TaskEvidenceCacheStoreAndHit) {
    agent::TaskEvidenceCache cache;
    cache.store(make_key(), make_pack("text"));
    const auto hit = cache.lookup(make_key());
    REQUIRE(hit.has_value());
    REQUIRE(hit->items.size() == 1);
    REQUIRE(hit->items.front().content == "text");
}

TEST_CASE(TaskEvidenceCacheRejectsDifferentQuery) {
    agent::TaskEvidenceCache cache;
    cache.store(make_key(), make_pack("text"));
    REQUIRE(!cache.lookup(make_key("different query")).has_value());
}

TEST_CASE(TaskEvidenceCacheRejectsDifferentRevision) {
    agent::TaskEvidenceCache cache;
    auto key = make_key();
    cache.store(key, make_pack("text"));
    key.retrieval_revision = "rev-B";
    REQUIRE(!cache.lookup(key).has_value());
}

TEST_CASE(TaskEvidenceCacheRejectsDifferentTaskId) {
    agent::TaskEvidenceCache cache;
    auto key = make_key();
    cache.store(key, make_pack("text"));
    key.task_id = "task-002";
    REQUIRE(!cache.lookup(key).has_value());
}

TEST_CASE(TaskEvidenceCacheRespectsByteCap) {
    agent::TaskEvidenceCache cache;
    const std::string huge(agent::TaskEvidenceCache::kMaxEvidenceBytes + 16, 'x');
    cache.store(make_key(), make_pack(huge));
    REQUIRE(cache.size() == 0);
    REQUIRE(!cache.lookup(make_key()).has_value());
}

TEST_CASE(TaskEvidenceCacheClear) {
    agent::TaskEvidenceCache cache;
    cache.store(make_key(), make_pack("text"));
    REQUIRE(cache.size() == 1);
    cache.clear();
    REQUIRE(cache.size() == 0);
    REQUIRE(!cache.lookup(make_key()).has_value());
}

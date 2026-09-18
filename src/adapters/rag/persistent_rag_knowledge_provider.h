#pragma once

#include "ports/jsonl_process.h"
#include "ports/knowledge_provider.h"
#include "ports/rag_pack_verifier.h"
#include "application/retrieval_policy.h"
#include "adapters/rag/task_evidence_cache.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

namespace agent {

struct RagConfig {
    bool enabled{false};
    std::string mode{"hybrid"};
    std::filesystem::path pack_root;
    std::size_t top_k{6};
    std::size_t max_total_bytes{32'768};
    std::int64_t startup_timeout_ms{120'000};
    std::int64_t query_timeout_ms{30'000};
    std::string device{"auto"};
    // T5: explicit retrieval policy from the user. The default
    // matches the previous behaviour (heuristic on every turn) so
    // existing configurations keep working.
    RetrievalPolicy retrieval_policy{RetrievalPolicy::Auto};
};

class PersistentRagKnowledgeProvider final : public KnowledgeProvider {
public:
    PersistentRagKnowledgeProvider(JsonlProcess& process,
                                   RagPackVerifier& pack_verifier,
                                   RagConfig config);
    ~PersistentRagKnowledgeProvider() override;

    PersistentRagKnowledgeProvider(const PersistentRagKnowledgeProvider&) = delete;
    PersistentRagKnowledgeProvider& operator=(
        const PersistentRagKnowledgeProvider&) = delete;

    Result<EvidencePack> retrieve(const TaskState& state,
                                 const OperationContext& context) override;
    using KnowledgeProvider::retrieve;

    // T5: classify the prompt before any work runs. The CLI uses
    // this to render phase messages and the runtime uses the
    // returned decision to skip retrieval entirely for chatty
    // turns.
    RetrievalDecision decide_turn(const std::string& issue,
                                  bool session_regulatory_context) const {
        return decide_retrieval(issue, config_.retrieval_policy,
                                session_regulatory_context);
    }

    // T5: ask the sidecar to prepare only the capability required
    // for the current turn. The default v3 path keeps the v2
    // behaviour so existing packs continue to work.
    void prepare_capability(RetrievalNeed need) noexcept;

    // T5: observed during the lifetime of the provider. Useful for
    // tests that need to verify capability negotiation without
    // poking at private state.
    bool embedding_ready() noexcept;
    bool index_ready() noexcept;

private:
    enum class State { Stopped, Starting, Ready, Broken };

    Result<void> ensure_ready();
    Result<void> ensure_capability(RetrievalNeed need);
    std::string next_request_id();
    void break_process() noexcept;

    JsonlProcess& process_;
    RagPackVerifier& pack_verifier_;
    RagConfig config_;
    std::mutex mutex_;
    State state_{State::Stopped};
    std::string retrieval_revision_;
    std::uint64_t request_sequence_{0};
    // T5: per-session carry flag. The runtime flips it true when a
    // turn needed retrieval and back to false when none of the last
    // N turns needed retrieval. The provider never reads or writes
    // this on its own.
    bool session_regulatory_context_{false};
    bool index_ready_{false};
    bool embedding_ready_{false};
    // T7: one-slot task evidence cache. Cleared on task end or when the
    // verifier rotates the pack.
    TaskEvidenceCache evidence_cache_;
};

}  // namespace agent

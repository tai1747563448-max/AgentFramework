#pragma once

#include "ports/jsonl_process.h"
#include "ports/knowledge_provider.h"

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
};

class PersistentRagKnowledgeProvider final : public KnowledgeProvider {
public:
    PersistentRagKnowledgeProvider(JsonlProcess& process, RagConfig config);
    ~PersistentRagKnowledgeProvider() override;

    PersistentRagKnowledgeProvider(const PersistentRagKnowledgeProvider&) = delete;
    PersistentRagKnowledgeProvider& operator=(
        const PersistentRagKnowledgeProvider&) = delete;

    Result<EvidencePack> retrieve(const TaskState& state) override;

private:
    enum class State { Stopped, Starting, Ready, Broken };

    Result<void> ensure_ready();
    std::string next_request_id();
    void break_process() noexcept;

    JsonlProcess& process_;
    RagConfig config_;
    std::mutex mutex_;
    State state_{State::Stopped};
    std::uint64_t request_sequence_{0};
};

}  // namespace agent

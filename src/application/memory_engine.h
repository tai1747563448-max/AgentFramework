#pragma once

#include "domain/memory_event.h"
#include "ports/memory_consolidator.h"

#include <cstddef>
#include <string>
#include <vector>

namespace agent {

class Clock;
class IdGenerator;
class MemoryStore;
class SessionStore;
class MemoryRetriever;
class MemoryPolicy;

struct MemoryConsolidationResult {
    std::size_t appended_entries{0};
    std::uint64_t through_turn{0};
    bool model_called{false};
};

class MemoryEngine {
public:
    MemoryEngine(MemoryStore& memories, SessionStore& sessions,
                 MemoryConsolidator& consolidator, MemoryRetriever& retriever,
                 MemoryPolicy& policy, Clock& clock, IdGenerator& ids);

    Result<std::vector<MemoryEntry>> list(const std::string& workspace_utf8) const;
    Result<MemoryEntry> remember(const std::string& session_id,
                                 const std::string& content);
    Result<void> forget(const std::string& memory_id);
    Result<std::vector<std::string>> retrieve(const std::string& workspace_utf8,
                                             const std::string& query,
                                             std::size_t top_k,
                                             std::size_t injected_byte_budget) const;
    Result<MemoryConsolidationResult> consolidate(const std::string& session_id);

private:
    Result<MemoryState> append_event(MemoryEventPayload payload);
    Result<MemoryEntry> append_entry(const MemoryCandidate& candidate,
                                     const std::string& session_id,
                                     std::uint64_t source_turn_start,
                                     std::uint64_t source_turn_end,
                                     MemoryOrigin origin);

    MemoryStore& memories_;
    SessionStore& sessions_;
    MemoryConsolidator& consolidator_;
    MemoryRetriever& retriever_;
    MemoryPolicy& policy_;
    Clock& clock_;
    IdGenerator& ids_;
};

} // namespace agent

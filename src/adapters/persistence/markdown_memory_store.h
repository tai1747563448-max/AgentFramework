#pragma once

// T16 (v2 §3): memdir filesystem memory. Each memory lives in its
// own markdown file at <root>/memories/<memory_id>.md with YAML-style
// frontmatter for the metadata fields. Reading a single memory is
// an O(1) file open; @mem:abc123 lookup is a path lookup rather than
// a full event log replay.
//
// The markdown store does NOT replace the JSONL event log — that log
// stays as the canonical audit trail and replay source. The markdown
// files are a read-optimised projection; they are rewritten whenever
// the corresponding MemoryEvent commits so the human-facing view stays
// in sync with the durable history.
//
// The interface mirrors MemoryStore so callers can swap stores by
// pointer; the per-id lookup is the main extension over the JSONL
// interface.
#include "ports/memory_store.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>

namespace agent {

class MarkdownMemoryStore final : public MemoryStore {
public:
    explicit MarkdownMemoryStore(std::filesystem::path runtime_root);

    // MemoryStore interface — same as JsonlMemoryStore. The JSONL log
    // remains the source of truth; the markdown store mirrors it.
    Result<void> append(const MemoryEvent& event) override;
    Result<std::vector<MemoryEvent>> read_all() const override;
    Result<MemoryState> read_state() const override;

    // Per-id lookup used by @mem:<id> resolution. Returns the parsed
    // markdown body (after the frontmatter) so callers do not need to
    // re-parse the file themselves.
    Result<std::string> load(const std::string& memory_id) const;

    // Directory the store reads from. Exposed for /memories output
    // and diagnostic logs.
    std::filesystem::path directory() const;

private:
    Result<std::filesystem::path> memory_path(const std::string& memory_id) const;
    void rewrite_entry(const MemoryEntry& entry);
    void remove_entry(const std::string& memory_id);

    std::filesystem::path runtime_root_;
    mutable std::mutex mutex_;
};

}  // namespace agent
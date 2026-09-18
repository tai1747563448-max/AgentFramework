#pragma once

#include "ports/event_store.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

namespace agent {

class JsonlEventStore final : public EventStore {
public:
    explicit JsonlEventStore(std::filesystem::path runtime_root);

    Result<void> append(const RuntimeEvent& event) override;
    Result<std::vector<RuntimeEvent>> read_file(
        const std::filesystem::path& path) const override;
    Result<std::vector<RuntimeEvent>> read_task(
        const std::string& task_id) const;
    Result<std::filesystem::path> event_path(
        const std::string& task_id) const;

    // T07: flush_pending writes any buffered WAL bytes for task_id to disk
    // and updates the per-task committed_through index. It is called by
    // the runtime engine every 32 events or 500ms; the path is idempotent.
    Result<void> flush_pending(const std::string& task_id);

    // T07: highest event sequence confirmed durable on disk for task_id.
    // Zero before any flush has happened. Used to bound the recovery window
    // when scanning for gaps after a crash.
    std::uint64_t committed_through(const std::string& task_id) const;

private:
    std::filesystem::path runtime_root_;
    mutable std::mutex index_mutex_;
    // Per-task flush bookkeeping. The counter rolls every 32 writes and
    // triggers an explicit fsync; the index captures the last sequence the
    // kernel confirmed durable.
    std::unordered_map<std::string, std::uint64_t> pending_counts_;
    std::unordered_map<std::string, std::uint64_t> committed_through_;
};

}  // namespace agent

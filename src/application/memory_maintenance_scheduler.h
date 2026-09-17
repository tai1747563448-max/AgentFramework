#pragma once

#include "domain/memory_event.h"
#include "domain/result.h"
#include "ports/memory_consolidator.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace agent {

class MemoryEngine;
class Clock;

// The scheduler accepts a (session_id, through_sequence) tuple per request and
// runs the actual model extraction off the foreground path. The worker output
// is a list of candidates paired with the source sequence they were derived
// from; only the scheduler's commit step is permitted to append to the formal
// MemoryStore. Other readers and writers (remember/forget/list/etc.) remain
// synchronous and read-then-write the same store through MemoryEngine, but the
// scheduler is the single owner of commit-time version checks so a candidate
// derived from an outdated transcript can never resurrect a deleted memory.
//
// Cancellation is two-tiered:
//   * The owning session's foreground request triggers `preempt(session_id)`,
//     which wakes any worker currently extracting for that session and drops
//     pending requests that have not started yet.
//   * `forget(session_id, memory_id)` invalidates any in-flight candidate
//     batch for that session: the worker discards its result before commit.
//
// Drain at process exit is bounded by `drain_for_exit(timeout)`. The function
// does not wait on the model; it returns as soon as either the queue empties
// or the timeout expires, so the CLI never blocks on a flaky provider.
class MemoryMaintenanceScheduler {
public:
    struct Config {
        // Maximum number of in-flight worker threads. The plan requires the
        // extraction ModelClient be independent from the foreground chat one,
        // so workers share no mutable state with each other or with the CLI.
        std::size_t worker_threads{2};
        // Hard per-worker timeout used when the foreground request does not
        // supply a deadline. The scheduler never blocks longer than this.
        std::chrono::milliseconds worker_timeout{std::chrono::seconds(30)};
    };

    struct CommitResult {
        std::uint64_t appended_entries{0};
        std::uint64_t through_turn{0};
        bool model_called{false};
        bool invalidated{false};
    };

    // The scheduler takes ownership of the memory engine pointer (the pointer
    // must outlive the scheduler). The consolidator is the independent
    // extraction client owned by the scheduler itself, satisfying the "do not
    // share mutable state with the foreground chat ModelClient" requirement.
    MemoryMaintenanceScheduler(MemoryEngine& engine,
                               std::unique_ptr<MemoryConsolidator> consolidator,
                               Config config = {});
    ~MemoryMaintenanceScheduler();

    MemoryMaintenanceScheduler(const MemoryMaintenanceScheduler&) = delete;
    MemoryMaintenanceScheduler& operator=(const MemoryMaintenanceScheduler&) = delete;

    // Submit a maintenance request. Non-blocking. The scheduler takes the
    // higher of (current checkpoint, previous through_sequence) and the new
    // through_sequence so duplicate or older requests are coalesced.
    void request_maintenance(const std::string& session_id,
                             std::uint64_t through_sequence);

    // Pre-empt maintenance for the given session (e.g. when the foreground
    // request needs to write). Pending requests are dropped; in-flight workers
    // are woken and their results discarded.
    void preempt(const std::string& session_id);

    // Forget invalidates any in-flight candidate batch for the session. The
    // scheduler also records the forgotten id so that when a worker finally
    // reaches commit, the version check rejects any candidate that was
    // derived before the forget event landed.
    void forget(const std::string& session_id, const std::string& memory_id);

    // Returns the highest through_sequence that has not been committed yet.
    // Used by the CLI to print progress and by the exit path to know which
    // range is unfinished.
    std::optional<std::uint64_t> pending_through(
        const std::string& session_id) const;

    // Wait up to `timeout` for the queue to drain. Returns the number of
    // requests that finished. The function never blocks longer than the
    // timeout and never re-issues a request that was preempted.
    std::size_t drain_for_exit(std::chrono::milliseconds timeout);

    // For tests: number of pending requests.
    std::size_t pending_count() const;

    // For tests: number of currently-running workers.
    std::size_t active_workers() const;

private:
    struct Request {
        std::string session_id;
        std::uint64_t through_sequence{0};
    };

    struct SessionState {
        std::uint64_t checkpoint{0};
        std::uint64_t pending_through{0};
        bool has_pending{false};
        // Memory ids that were forgotten while a request was in flight. The
        // commit step checks these and refuses to resurrect them.
        std::vector<std::string> invalidated_memory_ids;
        // Set while a worker is processing this session. Pre-empt and forget
        // flip the flag so the worker can drop the result before commit.
        std::shared_ptr<std::atomic<bool>> inflight_cancel;
    };

    // The scheduler's three private helpers mirror the three steps of the
    // refactored MemoryEngine::consolidate. Each helper is called from a
    // single worker thread at a time; the engine itself enforces single-owner
    // commit semantics.
    Result<CommitResult> run_one(Request request);
    void worker_loop();

    MemoryEngine& engine_;
    std::unique_ptr<MemoryConsolidator> consolidator_;
    Config config_;

    mutable std::mutex state_mutex_;
    std::condition_variable state_changed_;
    std::deque<Request> queue_;
    std::map<std::string, SessionState> sessions_;
    std::size_t active_{0};
    bool stopping_{false};

    std::vector<std::thread> workers_;
};

} // namespace agent

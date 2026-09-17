#include "application/memory_maintenance_scheduler.h"

#include "application/memory_engine.h"
#include "ports/clock.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace agent {

MemoryMaintenanceScheduler::MemoryMaintenanceScheduler(
    MemoryEngine& engine, std::unique_ptr<MemoryConsolidator> consolidator,
    Config config)
    : engine_(engine),
      consolidator_(std::move(consolidator)),
      config_(std::move(config)) {
    if (config_.worker_threads == 0) config_.worker_threads = 1;
    if (config_.worker_timeout <= std::chrono::milliseconds(0))
        config_.worker_timeout = std::chrono::seconds(30);
    workers_.reserve(config_.worker_threads);
    for (std::size_t index = 0; index < config_.worker_threads; ++index) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

MemoryMaintenanceScheduler::~MemoryMaintenanceScheduler() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        stopping_ = true;
        // Wake any worker that is blocked in extract_candidates. The worker
        // checks the cancel flag before and after each step; flipping it here
        // ensures an in-flight worker drops its result before commit. The
        // destructor's `drain_for_exit` then bounds the wait so a stuck model
        // cannot deadlock process exit.
        for (auto& entry : sessions_) {
            if (entry.second.inflight_cancel) {
                entry.second.inflight_cancel->store(true);
            }
        }
    }
    state_changed_.notify_all();
    drain_for_exit(std::chrono::seconds(2));
    for (auto& worker : workers_) {
        if (worker.joinable()) worker.join();
    }
}

void MemoryMaintenanceScheduler::request_maintenance(
    const std::string& session_id, std::uint64_t through_sequence) {
    if (!is_valid_session_id(session_id)) return;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (stopping_) return;
        auto& state = sessions_[session_id];
        const auto target =
            std::max({state.checkpoint, state.pending_through, through_sequence});
        if (!state.has_pending || target > state.pending_through) {
            state.pending_through = target;
            state.has_pending = true;
        } else {
            // Already covered by an earlier request; no need to re-queue.
            return;
        }
        if (!state.inflight_cancel) {
            state.inflight_cancel =
                std::make_shared<std::atomic<bool>>(false);
        }
        state.inflight_cancel->store(false);
        Request request{session_id, state.pending_through};
        queue_.push_back(std::move(request));
    }
    state_changed_.notify_one();
}

void MemoryMaintenanceScheduler::preempt(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    // Drop queued requests for this session.
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                 [&session_id](const Request& request) {
                                     return request.session_id == session_id;
                                 }),
                 queue_.end());
    auto iterator = sessions_.find(session_id);
    if (iterator != sessions_.end()) {
        iterator->second.has_pending = false;
        iterator->second.pending_through = iterator->second.checkpoint;
        if (iterator->second.inflight_cancel) {
            iterator->second.inflight_cancel->store(true);
        }
    }
    state_changed_.notify_all();
}

void MemoryMaintenanceScheduler::forget(const std::string& session_id,
                                        const std::string& memory_id) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto& state = sessions_[session_id];
    state.invalidated_memory_ids.push_back(memory_id);
    // Drop any pending maintenance work for the session: the forget already
    // takes effect synchronously through MemoryEngine::forget, so a queued
    // worker would either race against that write or duplicate work.
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                 [&session_id](const Request& request) {
                                     return request.session_id == session_id;
                                 }),
                 queue_.end());
    state.has_pending = false;
    state.pending_through = state.checkpoint;
    if (state.inflight_cancel) {
        state.inflight_cancel->store(true);
    }
    state_changed_.notify_all();
}

std::optional<std::uint64_t> MemoryMaintenanceScheduler::pending_through(
    const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto iterator = sessions_.find(session_id);
    if (iterator == sessions_.end()) return std::nullopt;
    if (!iterator->second.has_pending) return std::nullopt;
    return iterator->second.pending_through;
}

std::size_t MemoryMaintenanceScheduler::drain_for_exit(
    std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::size_t completed = 0;
    std::unique_lock<std::mutex> lock(state_mutex_);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto idle = queue_.empty() && active_ == 0;
        if (idle && sessions_.empty()) break;
        if (idle) {
            bool any_pending = false;
            for (const auto& pair : sessions_) {
                if (pair.second.has_pending) {
                    any_pending = true;
                    break;
                }
            }
            if (!any_pending) break;
        }
        state_changed_.wait_until(lock, deadline);
    }
    // Count whatever did finish during the window. Active workers may still
    // be running when the deadline hits; the CLI is allowed to exit anyway
    // because the scheduler is bound to its own lifetime and persists the
    // unfinished range via the per-session checkpoint.
    completed = 0;
    for (const auto& pair : sessions_) {
        if (!pair.second.has_pending) ++completed;
    }
    return completed;
}

std::size_t MemoryMaintenanceScheduler::pending_count() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return queue_.size();
}

std::size_t MemoryMaintenanceScheduler::active_workers() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return active_;
}

Result<MemoryMaintenanceScheduler::CommitResult>
MemoryMaintenanceScheduler::run_one(Request request) {
    CommitResult outcome;
    outcome.through_turn = request.through_sequence;
    std::shared_ptr<std::atomic<bool>> cancel;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto iterator = sessions_.find(request.session_id);
        if (iterator != sessions_.end()) {
            cancel = iterator->second.inflight_cancel;
        }
    }
    // Step 1: read the fixed transcript.
    const auto transcript =
        engine_.read_maintenance_transcript(request.session_id,
                                            request.through_sequence);
    if (!transcript.has_value()) {
        // Persistent failure: keep the checkpoint where it is and let the
        // next request retry.
        return Result<CommitResult>::success(outcome);
    }
    outcome.through_turn = transcript.value().through_turn;
    if (transcript.value().checkpoint >= transcript.value().through_turn) {
        // Already caught up.
        return Result<CommitResult>::success(outcome);
    }
    if (cancel && cancel->load()) {
        outcome.invalidated = true;
        return Result<CommitResult>::success(outcome);
    }
    const auto batches = engine_.build_batches(transcript.value());
    std::vector<std::string> invalidated_snapshot;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto iterator = sessions_.find(request.session_id);
        if (iterator != sessions_.end()) {
            invalidated_snapshot = iterator->second.invalidated_memory_ids;
        }
    }
    for (const auto& batch : batches) {
        if (cancel && cancel->load()) {
            outcome.invalidated = true;
            return Result<CommitResult>::success(outcome);
        }
        if (batch.turns.empty()) {
            // Empty batch is a checkpoint-only advance (oversized turn or
            // trailing tail). It still needs a commit so the next request
            // doesn't redo the work.
            auto commit = engine_.commit_maintenance(
                request.session_id, {}, batch, invalidated_snapshot);
            if (!commit.has_value())
                return Result<CommitResult>::failure(commit.error());
            outcome.appended_entries += commit.value().appended_entries;
            continue;
        }
        auto candidates = engine_.extract_candidates(request.session_id,
                                                     transcript.value(),
                                                     batch, cancel.get());
        if (!candidates.has_value()) {
            const auto& err = candidates.error();
            if (err.code == ErrorCode::Cancelled) {
                outcome.invalidated = true;
                return Result<CommitResult>::success(outcome);
            }
            return Result<CommitResult>::failure(
                {err.code, "memory consolidation failed", err.retryable});
        }
        if (cancel && cancel->load()) {
            outcome.invalidated = true;
            return Result<CommitResult>::success(outcome);
        }
        outcome.model_called = true;
        auto commit = engine_.commit_maintenance(request.session_id,
                                                 candidates.value(), batch,
                                                 invalidated_snapshot);
        if (!commit.has_value())
            return Result<CommitResult>::failure(commit.error());
        outcome.appended_entries += commit.value().appended_entries;
    }
    return Result<CommitResult>::success(outcome);
}

void MemoryMaintenanceScheduler::worker_loop() {
    for (;;) {
        Request request;
        {
            std::unique_lock<std::mutex> lock(state_mutex_);
            state_changed_.wait(lock, [this] {
                return stopping_ || !queue_.empty();
            });
            if (stopping_ && queue_.empty()) return;
            request = std::move(queue_.front());
            queue_.pop_front();
            ++active_;
        }
        // Run outside the lock so concurrent CLI requests can be queued.
        const auto deadline =
            std::chrono::steady_clock::now() + config_.worker_timeout;
        CommitResult outcome;
        bool ok = false;
        try {
            const auto result = run_one(request);
            if (result.has_value()) {
                outcome = result.value();
                ok = true;
            }
        } catch (...) {
            ok = false;
        }
        const auto exceeded =
            std::chrono::steady_clock::now() > deadline && !ok;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            --active_;
            auto iterator = sessions_.find(request.session_id);
            if (iterator == sessions_.end()) {
                // Session was forgotten mid-flight; nothing to record.
                state_changed_.notify_all();
                continue;
            }
            auto& state = iterator->second;
            if (ok && !outcome.invalidated && !exceeded) {
                // Read the persisted checkpoint to advance the cached view.
                // We do not have direct access to the engine here, so the
                // scheduler defers the exact through-turn to the engine's
                // own bookkeeping: the session is "caught up to" its request
                // through_sequence, which is the strongest promise we can
                // make without re-reading the file under the lock.
                state.checkpoint = std::max<std::uint64_t>(
                    state.checkpoint, request.through_sequence);
                state.has_pending = false;
            }
            // Whether or not the run succeeded, forget invalidations applied
            // for this batch are consumed so the next batch can commit.
            if (ok && !outcome.invalidated) {
                state.invalidated_memory_ids.clear();
            }
            state_changed_.notify_all();
        }
    }
}

} // namespace agent

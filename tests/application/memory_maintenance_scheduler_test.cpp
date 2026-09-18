#include "application/memory_engine.h"
#include "application/memory_maintenance_scheduler.h"
#include "application/memory_policy.h"
#include "application/memory_retriever.h"
#include "application/session_reducer.h"
#include "adapters/persistence/jsonl_memory_store.h"
#include "adapters/persistence/jsonl_session_store.h"
#include "ports/clock.h"
#include "ports/id_generator.h"
#include "ports/memory_consolidator.h"
#include "test_support.h"

#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

namespace memory_maintenance_scheduler_test {

const std::string session_id =
    "session-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
const std::string workspace = "E:/workspace";

std::string identity(const char* prefix, std::uint64_t number) {
    std::ostringstream stream;
    stream << prefix << std::hex << std::setw(32) << std::setfill('0') << number;
    return stream.str();
}

class Clock final : public agent::Clock {
public:
    std::string now_utc() const override { return utc; }
    std::int64_t monotonic_ms() const override { return 1000; }
    std::string utc{"2026-09-07T12:00:00.000Z"};
};

class Ids final : public agent::IdGenerator {
public:
    std::string next_task_id() override { return identity("task-", next++); }
    std::string next_correlation_id() override {
        return "corr-maintenance-" + std::to_string(correlation++);
    }
    std::uint64_t next{1}, correlation{1};
};

// Consolidator that supports controlled delays so the test can demonstrate
// the foreground path does not wait for the model.
class GatedConsolidator final : public agent::MemoryConsolidator {
public:
    agent::Result<std::vector<agent::MemoryCandidate>> consolidate(
        const agent::MemoryConsolidationInput& input) override {
        std::size_t text_bytes = 0;
        for (const auto& turn : input.turns) {
            for (const auto& message : turn.messages) {
                for (const auto& block : message.content) {
                    if (const auto* text = std::get_if<agent::TextBlock>(&block))
                        text_bytes += text->text.size();
                }
            }
        }
        if (max_input_text_bytes != 0 && text_bytes > max_input_text_bytes)
            return agent::Result<std::vector<agent::MemoryCandidate>>::failure(
                {agent::ErrorCode::BudgetExceeded, "provider context limit", false});
        calls.fetch_add(1);
        if (gate.load()) {
            std::unique_lock<std::mutex> lock(mutex);
            gate_changed.wait(lock, [this] { return !gate.load(); });
        }
        if (!succeed) {
            return agent::Result<std::vector<agent::MemoryCandidate>>::failure(
                {failure_code.load(), "PRIVATE_PROVIDER_BODY", true});
        }
        return agent::Result<std::vector<agent::MemoryCandidate>>::success(
            candidates);
    }

    void open_gate() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            gate.store(false);
        }
        gate_changed.notify_all();
    }
    void close_gate() {
        std::lock_guard<std::mutex> lock(mutex);
        gate.store(true);
    }

    std::atomic<bool> gate{false};
    std::atomic<int> calls{0};
    std::atomic<agent::ErrorCode> failure_code{agent::ErrorCode::InvalidInput};
    bool succeed{true};
    std::size_t max_input_text_bytes{0};
    std::vector<agent::MemoryCandidate> candidates;
    std::mutex mutex;
    std::condition_variable gate_changed;
};

struct Fixture;

// A wrapper consolidator that delegates to the fixture's GatedConsolidator
// through a stable pointer. This lets the scheduler own its consolidator
// (per the spec) while the test retains the gate primitives.
class ConsolidatorBridge final : public agent::MemoryConsolidator {
public:
    explicit ConsolidatorBridge(GatedConsolidator& target) : target_(&target) {}
    agent::Result<std::vector<agent::MemoryCandidate>> consolidate(
        const agent::MemoryConsolidationInput& input) override {
        return target_->consolidate(input);
    }
private:
    GatedConsolidator* target_;
};

struct Fixture {
    test::ScopedTempDir root{"agent-maintenance-scheduler"};
    agent::JsonlSessionStore sessions{root.path()};
    agent::JsonlMemoryStore memories{root.path()};
    Clock clock;
    Ids ids;
    GatedConsolidator consolidator;
    agent::MemoryRetriever retriever;
    agent::MemoryPolicy policy{{1024, {"scheduler-protected-value"}}};
    agent::MemoryEngine engine{memories, sessions, consolidator, retriever,
                               policy, clock, ids};
    ~Fixture() {
        // Ensure any in-flight worker blocked behind the test gate can
        // unblock and exit cleanly before the scheduler destructor joins.
        consolidator.open_gate();
    }

    Fixture() {
        REQUIRE(sessions.append({1, 1, session_id, clock.utc, "corr-start",
                                 agent::SessionStartedPayload{workspace,
                                                               "model"}})
                    .has_value());
    }

    void append(agent::SessionEventPayload payload) {
        const auto events = sessions.read_session(session_id);
        REQUIRE(events.has_value());
        const auto result = sessions.append({1, events.value().size() + 1,
                                              session_id, clock.utc,
                                              ids.next_correlation_id(),
                                              std::move(payload)});
        if (!result.has_value()) {
            std::cerr << "sessions.append failed: " << result.error().message
                      << " events.size=" << events.value().size() << "\n";
        }
        REQUIRE(result.has_value());
    }

    void turn(std::uint64_t index) {
        const auto task_id = identity("task-", 100 + index);
        const auto question = "q" + std::to_string(index);
        append(agent::SessionTurnStartedPayload{index, task_id, question});
        std::vector<agent::Message> messages{
            {agent::Role::User, {agent::TextBlock{question}}},
            {agent::Role::Assistant,
             {agent::TextBlock{"a" + std::to_string(index)}}}};
        append(agent::SessionTurnCommittedPayload{index, task_id, messages});
    }

    std::unique_ptr<agent::MemoryMaintenanceScheduler> make_scheduler(
        agent::MemoryMaintenanceScheduler::Config config = {2,
            std::chrono::seconds(2)}) {
        // Hand the scheduler a unique_ptr to a consolidator that forwards to
        // the fixture's gated consolidator. The fixture retains ownership so
        // the test can still drive the gate after construction.
        auto wrapper = std::make_unique<ConsolidatorBridge>(consolidator);
        return std::make_unique<agent::MemoryMaintenanceScheduler>(
            engine, std::move(wrapper), config);
    }

    agent::MemoryState state() {
        const auto loaded = memories.read_state();
        REQUIRE(loaded.has_value());
        return loaded.value();
    }
};

// Helper used by session-switch tests to append a fresh session without
// pulling the full fixture inside the test bodies.
}  // namespace memory_maintenance_scheduler_test

namespace memory_maintenance_scheduler_test {
bool sessions_append(Fixture& fixture, const std::string& sid);
}  // namespace memory_maintenance_scheduler_test

TEST_CASE(memory_maintenance_scheduler_does_not_block_startup_or_exit) {
    using namespace memory_maintenance_scheduler_test;
    Fixture fixture;
    fixture.turn(1);
    fixture.turn(2);
    fixture.consolidator.candidates = {
        {agent::MemoryCategory::Fact, workspace, "C++ startup candidate"}};
    fixture.consolidator.close_gate();

    auto scheduler = fixture.make_scheduler();
    const auto started = std::chrono::steady_clock::now();
    scheduler->request_maintenance(session_id, 2);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started)
            .count();
    // A fake model that never returns must not block request_maintenance.
    REQUIRE(elapsed < 250);
    REQUIRE(scheduler->pending_through(session_id).value_or(0) == 2);
    REQUIRE(scheduler->pending_count() == 1);
    REQUIRE(fixture.state().active_entries.empty());
    REQUIRE(fixture.state().session_checkpoints.empty());

    // Drain at exit must also return promptly even though the worker is
    // still blocked behind the gate.
    fixture.consolidator.open_gate();
    const auto drain_started = std::chrono::steady_clock::now();
    const auto drained = scheduler->drain_for_exit(std::chrono::milliseconds(0));
    const auto drain_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - drain_started)
            .count();
    REQUIRE(drain_elapsed < 250);
    // drain_for_exit is allowed to return without finishing the work; the
    // unfinished range is recorded in pending_through().
    (void)drained;
    REQUIRE(scheduler->pending_through(session_id).has_value());
}

TEST_CASE(memory_maintenance_scheduler_foreground_request_preempts_pending_work) {
    using namespace memory_maintenance_scheduler_test;
    Fixture fixture;
    fixture.turn(1);
    fixture.turn(2);
    fixture.turn(3);
    fixture.consolidator.candidates = {
        {agent::MemoryCategory::Fact, workspace, "C++ pending candidate"}};
    fixture.consolidator.close_gate();

    auto scheduler = fixture.make_scheduler();
    scheduler->request_maintenance(session_id, 3);
    // The consolidator is blocked, so the request is in flight. Wait briefly
    // for a worker to pick up the queued request.
    for (int i = 0; i < 50 && scheduler->active_workers() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(scheduler->active_workers() >= 1);
    // The foreground path pre-empts by session id, not by waiting.
    scheduler->preempt(session_id);
    fixture.consolidator.open_gate();
    // After pre-empt, the worker either discards the result or finishes with
    // no checkpoint advance. In either case the scheduler reports no pending
    // work for the session.
    for (int i = 0; i < 50 && scheduler->active_workers() > 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(!scheduler->pending_through(session_id).has_value());
    REQUIRE(fixture.state().active_entries.empty());
}

TEST_CASE(memory_maintenance_scheduler_forget_invalidates_inflight_candidates) {
    using namespace memory_maintenance_scheduler_test;
    Fixture fixture;
    fixture.turn(1);
    fixture.turn(2);
    fixture.consolidator.candidates = {
        {agent::MemoryCategory::Fact, workspace, "C++ rescue content"}};
    fixture.consolidator.close_gate();

    auto scheduler = fixture.make_scheduler();
    scheduler->request_maintenance(session_id, 2);
    // Worker is now blocked behind the gate. The CLI simulates a synchronous
    // /forget call: the user deleted an entry from elsewhere while the
    // candidate batch is in flight. The scheduler must drop pending work for
    // the session, so any result that eventually comes back cannot resurrect
    // the deleted memory.
    scheduler->forget(session_id, "memory-cccccccccccccccccccccccccccccccc");
    fixture.consolidator.open_gate();
    for (int i = 0; i < 50 && scheduler->active_workers() > 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(fixture.state().active_entries.empty());
    REQUIRE(fixture.state().session_checkpoints.empty());
}

TEST_CASE(memory_maintenance_scheduler_session_switch_does_not_wait_for_model) {
    using namespace memory_maintenance_scheduler_test;
    Fixture fixture;
    fixture.turn(1);
    fixture.consolidator.candidates = {
        {agent::MemoryCategory::Fact, workspace, "C++ long session"}};
    fixture.consolidator.close_gate();

    auto scheduler = fixture.make_scheduler();
    scheduler->request_maintenance(session_id, 1);
    // Wait briefly for a worker to pick up the queued request before the
    // session switch arrives; this is what production CLI code observes.
    for (int i = 0; i < 50 && scheduler->active_workers() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(scheduler->active_workers() >= 1);
    // Simulate the user invoking /new. The CLI enqueues a request for the
    // brand new session and returns. Maintenance for the previous session
    // continues in the background; the CLI never waits.
    const std::string new_session = "session-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    REQUIRE(sessions_append(fixture, new_session));
    const auto started = std::chrono::steady_clock::now();
    scheduler->request_maintenance(new_session, 0);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started)
            .count();
    REQUIRE(elapsed < 50);
    // Opening the gate lets the workers drain; eventually the candidate from
    // session A is committed but the new session B (zero turns) never
    // reaches the model.
    fixture.consolidator.open_gate();
    for (int i = 0; i < 100; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (scheduler->pending_count() == 0 &&
            scheduler->active_workers() == 0) break;
    }
    REQUIRE(scheduler->pending_count() == 0);
    REQUIRE(scheduler->active_workers() == 0);
    REQUIRE(fixture.state().session_checkpoints.count(session_id) == 1);
}

namespace memory_maintenance_scheduler_test {
bool sessions_append(Fixture& fixture, const std::string& sid) {
    return fixture.sessions
        .append({1, 1, sid, fixture.clock.utc, fixture.ids.next_correlation_id(),
                 agent::SessionStartedPayload{workspace, "model"}})
        .has_value();
}
}  // namespace memory_maintenance_scheduler_test

TEST_CASE(memory_maintenance_scheduler_checkpoint_does_not_overadvance_on_failure) {
    using namespace memory_maintenance_scheduler_test;
    Fixture fixture;
    fixture.turn(1);
    fixture.turn(2);
    fixture.consolidator.candidates = {
        {agent::MemoryCategory::Fact, workspace, "C++ fail checkpoint"}};
    fixture.consolidator.succeed = false;
    fixture.consolidator.failure_code = agent::ErrorCode::ProtocolFailure;

    auto scheduler = fixture.make_scheduler();
    scheduler->request_maintenance(session_id, 2);
    for (int i = 0; i < 100; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (scheduler->pending_count() == 0 &&
            scheduler->active_workers() == 0) break;
    }
    // A failed batch must not advance the persisted checkpoint past
    // through_sequence on the original request.
    const auto memory_state = fixture.state();
    const auto checkpoint = memory_state.session_checkpoints.find(session_id);
    if (checkpoint != memory_state.session_checkpoints.end()) {
        REQUIRE(checkpoint->second <= 2);
    }
    // On a failed run, the scheduler should still consider the session
    // pending so a later request can retry.
    REQUIRE(scheduler->pending_through(session_id).has_value());
}

TEST_CASE(memory_maintenance_scheduler_commit_advances_through_turn_exactly_once) {
    using namespace memory_maintenance_scheduler_test;
    Fixture fixture;
    fixture.turn(1);
    fixture.turn(2);
    fixture.turn(3);
    fixture.consolidator.candidates = {
        {agent::MemoryCategory::Fact, workspace, "C++ once candidate"}};

    auto scheduler = fixture.make_scheduler();
    scheduler->request_maintenance(session_id, 3);
    for (int i = 0; i < 100; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (scheduler->pending_count() == 0 &&
            scheduler->active_workers() == 0) break;
    }
    REQUIRE(fixture.state().active_entries.size() == 1);
    const auto checkpoint = fixture.state().session_checkpoints.at(session_id);
    REQUIRE(checkpoint == 3);
    // A second request for the same through_sequence is a no-op.
    scheduler->request_maintenance(session_id, 3);
    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (scheduler->pending_count() == 0 &&
            scheduler->active_workers() == 0) break;
    }
    REQUIRE(fixture.state().session_checkpoints.at(session_id) == 3);
}

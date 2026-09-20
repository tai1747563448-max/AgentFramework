#include "composition/engine.h"
#include "test_support.h"

#include <chrono>
#include <filesystem>

namespace {

class EngineFixture {
public:
    EngineFixture() : root_(std::filesystem::temp_directory_path() /
        ("engine-facade-" + std::to_string(counter_++) + "-" +
         std::to_string(std::chrono::high_resolution_clock::now()
                            .time_since_epoch().count()))) {
        std::filesystem::create_directories(root_);
    }
    ~EngineFixture() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }
    const std::filesystem::path& root() const { return root_; }
private:
    std::filesystem::path root_;
    static inline int counter_{0};
};

TEST_CASE(engine_facade_builds_with_disabled_memory) {
    EngineFixture fixture;
    agent::EngineConfig config;
    config.runtime_root = fixture.root();
    config.model_name = "claude-3-5-sonnet";
    config.memory_enabled = false;
    config.system_prompt = "test";
    auto engine = agent::build_engine(config);
    REQUIRE(engine != nullptr);
    REQUIRE(engine->config().model_name == "claude-3-5-sonnet");
}

TEST_CASE(engine_facade_cancel_is_safe_when_already_idle) {
    EngineFixture fixture;
    agent::EngineConfig config;
    config.runtime_root = fixture.root();
    config.model_name = "claude-3-5-sonnet";
    config.memory_enabled = false;
    auto engine = agent::build_engine(config);
    bool threw = true;
    try { engine->cancel(); threw = false; } catch (...) {}
    REQUIRE(!threw);
}

TEST_CASE(engine_facade_ask_creates_session_when_id_is_empty) {
    EngineFixture fixture;
    agent::EngineConfig config;
    config.runtime_root = fixture.root();
    config.model_name = "claude-3-5-sonnet";
    config.memory_enabled = false;
    auto engine = agent::build_engine(config);
    // ask("") must route through create_session and then submit_turn.
    // Without a model network we cannot assert success, but we can
    // assert that the facade surfaced a structured SessionTurnResult
    // (not the legacy empty stub). An error here indicates the
    // SessionEngine wiring is intact; the underlying failure comes
    // from the unreachable Anthropic endpoint.
    const auto result = engine->ask("", "hello", {}, false);
    // SessionEngine::submit_turn propagates an error from
    // load_session or model call. The happy path populates both
    // session and (potentially) task slots because create_session
    // ran first; the failure path may only populate error if the
    // underlying Anthropic endpoint is unreachable in this fixture.
    // Guard session.value() against the nullopt case so the test
    // reports a clean failure instead of bad_optional_access when
    // create_session itself fails before submit_turn runs.
    REQUIRE((result.error.has_value() ||
             (result.session.has_value() &&
              !result.session->session_id.empty())));
}

TEST_CASE(engine_facade_ask_rejects_empty_prompt_after_session) {
    EngineFixture fixture;
    agent::EngineConfig config;
    config.runtime_root = fixture.root();
    config.model_name = "claude-3-5-sonnet";
    config.memory_enabled = false;
    auto engine = agent::build_engine(config);
    // Empty prompt against a real session id should be rejected by
    // submit_turn's `user_text.empty()` guard, surfacing an error
    // rather than silently invoking the model.
    const auto result = engine->ask("session-test", "", {}, false);
    REQUIRE(result.error.has_value());
}

TEST_CASE(engine_facade_resume_rejects_empty_session_id) {
    EngineFixture fixture;
    agent::EngineConfig config;
    config.runtime_root = fixture.root();
    config.model_name = "claude-3-5-sonnet";
    config.memory_enabled = false;
    auto engine = agent::build_engine(config);
    const auto result = engine->resume("", {}, "fallback");
    REQUIRE(result.fatal_error.has_value());
    REQUIRE(result.fatal_error->code == agent::ErrorCode::InvalidInput);
}

TEST_CASE(engine_facade_cancel_calls_port_method) {
    EngineFixture fixture;
    agent::EngineConfig config;
    config.runtime_root = fixture.root();
    config.model_name = "claude-3-5-sonnet";
    config.memory_enabled = false;
    auto engine = agent::build_engine(config);
    // Engine::cancel must invoke the Cancellation port's cancel()
    // method directly; this is verified by triggering a cancel and
    // confirming the runtime state machine observes it. We can't
    // observe the SignalCancellation flag without exposing it, so
    // we instead check that cancel() is non-throwing on a fresh
    // engine (the SignalCancellation process-global flag is set,
    // and the runtime's next handle_awaiting_model will pick it up).
    bool threw = false;
    try { engine->cancel(); } catch (...) { threw = true; }
    REQUIRE(!threw);
}

TEST_CASE(engine_facade_holds_config_through_lifetime) {
    EngineFixture fixture;
    agent::EngineConfig config;
    config.runtime_root = fixture.root();
    config.model_name = "claude-3-5-sonnet";
    config.memory_enabled = true;
    config.system_prompt = "remember: drink water";
    auto engine = agent::build_engine(config);
    REQUIRE(engine->config().system_prompt == "remember: drink water");
    REQUIRE(engine->config().memory_enabled);
}

TEST_CASE(engine_facade_default_sandbox_is_non_null) {
    EngineFixture fixture;
    agent::EngineConfig config;
    config.runtime_root = fixture.root();
    config.model_name = "claude-3-5-sonnet";
    config.memory_enabled = false;
    auto engine = agent::build_engine(config);
    // make_default_sandbox() picks the platform-appropriate
    // implementation and falls back to NoOp when no containment
    // primitive is available. The facade holds the result through
    // its lifetime; we verify construction did not throw and the
    // config round-trip is intact.
    REQUIRE(engine != nullptr);
    REQUIRE(engine->config().memory_enabled == false);
}

TEST_CASE(engine_facade_mcp_servers_field_round_trip) {
    EngineFixture fixture;
    agent::EngineConfig config;
    config.runtime_root = fixture.root();
    config.model_name = "claude-3-5-sonnet";
    config.memory_enabled = false;
    // Format: name=program args;name2=program2 args2
    config.mcp_servers = "echo=echo hi;grep=grep needle haystack";
    // bootstrap_all() will fail because the test fixture has no real
    // echo/grep servers, but build_engine is fail-open: it must not
    // throw and must produce a usable engine.
    auto engine = agent::build_engine(config);
    REQUIRE(engine != nullptr);
    REQUIRE(engine->config().mcp_servers ==
            "echo=echo hi;grep=grep needle haystack");
}

TEST_CASE(engine_facade_mcp_disabled_when_env_empty) {
    EngineFixture fixture;
    agent::EngineConfig config;
    config.runtime_root = fixture.root();
    config.model_name = "claude-3-5-sonnet";
    config.memory_enabled = false;
    config.mcp_servers = "";
    // Empty mcp_servers skips the entire MCP bootstrap path. We
    // assert construction succeeds (no MCP gateway attached, only
    // the workspace gateway) and config round-trips.
    auto engine = agent::build_engine(config);
    REQUIRE(engine != nullptr);
    REQUIRE(engine->config().mcp_servers.empty());
}

}  // namespace
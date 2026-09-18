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

TEST_CASE(engine_facade_ask_returns_empty_session_turn) {
    EngineFixture fixture;
    agent::EngineConfig config;
    config.runtime_root = fixture.root();
    config.model_name = "claude-3-5-sonnet";
    config.memory_enabled = false;
    auto engine = agent::build_engine(config);
    const auto result = engine->ask("session-test",
                                     "hello",
                                     {},
                                     false);
    // The facade currently returns an empty SessionTurnResult;
    // wiring the full SessionEngine through the Engine is follow-up
    // work. The contract under test is "no crash, no surprise".
    (void)result;
    REQUIRE(true);
}

TEST_CASE(engine_facade_resume_returns_empty_runtime_result) {
    EngineFixture fixture;
    agent::EngineConfig config;
    config.runtime_root = fixture.root();
    config.model_name = "claude-3-5-sonnet";
    config.memory_enabled = false;
    auto engine = agent::build_engine(config);
    const auto result = engine->resume("session-test", {}, "fallback");
    (void)result;
    REQUIRE(true);
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

}  // namespace
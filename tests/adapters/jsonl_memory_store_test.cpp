#include "adapters/persistence/jsonl_memory_store.h"
#include "adapters/persistence/memory_event_json.h"
#include "test_support.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>

namespace {
constexpr const char *id = "memory-11111111111111111111111111111111";
agent::MemoryEvent first() {
    return {1, 1, "2026-09-07T10:00:00.000Z", "corr",
            agent::MemoryUpsertedPayload{
                {id, agent::MemoryCategory::Fact, "", "Use C++17",
                 "session-11111111111111111111111111111111", 1, 1,
                 "2026-09-07T10:00:00.000Z", "2026-09-07T10:00:00.000Z",
                 agent::MemoryOrigin::ExplicitUser}}};
}
std::string bytes(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
} // namespace

TEST_CASE(memory_store_missing_log_is_empty_then_secure_append_replays_state) {
    test::ScopedTempDir temp("memory-roundtrip");
    const auto root = temp.path() / "new-runtime";
    agent::JsonlMemoryStore store(root);
    const auto missing = store.read_all();
    if (!missing.has_value())
        std::cout << "Missing log read failed: " << missing.error().message << "\n";
    REQUIRE(missing.has_value());
    REQUIRE(store.read_all().value().empty());
    REQUIRE(store.read_state().has_value());
    REQUIRE(store.read_state().value() == agent::MemoryState{});
    REQUIRE(!std::filesystem::exists(root));
    REQUIRE(store.append(first()).has_value());
    REQUIRE(store.event_path().value() == root / "memories" / "events.jsonl");
    REQUIRE(store.read_all().value() == std::vector<agent::MemoryEvent>{first()});
    REQUIRE(store.read_state().value().active_entries.at(id).content == "Use C++17");
    const auto before = bytes(store.event_path().value());
    REQUIRE(!store.append(first()).has_value());
    REQUIRE(bytes(store.event_path().value()) == before);
    const agent::MemoryEvent forgotten{1, 2, "2026-09-07T10:00:01.000Z", "corr",
                                       agent::MemoryForgottenPayload{id}};
    REQUIRE(store.append(forgotten).has_value());
    REQUIRE(store.read_state().value().active_entries.empty());
    REQUIRE(bytes(store.event_path().value()).substr(0, before.size()) == before);
}

TEST_CASE(memory_store_rejects_corruption_without_changing_existing_bytes) {
    test::ScopedTempDir temp("memory-corrupt");
    agent::JsonlMemoryStore store(temp.path());
    const auto valid = agent::memory_event_to_json(first()).dump();
    std::vector<std::string> invalid{
        "",       "\n",  "{}\n",         "[]\n",        "null\n",
        "true\n", valid, valid + "\n\n", valid + "\n{", valid + "\n" + valid + "\n"};
    for (const std::string member :
         {"\"sequence\":1", "\"entry\":", "\"content\":\"Use C++17\""}) {
        auto duplicate = valid;
        const auto position = duplicate.find(member);
        REQUIRE(position != std::string::npos);
        if (member == "\"entry\":") {
            duplicate.insert(position, "\"entry\":{},");
        } else {
            duplicate.insert(position, member + ",");
        }
        invalid.push_back(duplicate + "\n");
    }
    for (const auto &corrupt : invalid) {
        const auto path = temp.write_text("memories/events.jsonl", corrupt);
        REQUIRE(!store.read_all().has_value());
        REQUIRE(!store.read_state().has_value());
        REQUIRE(!store.append(first()).has_value());
        REQUIRE(bytes(path) == corrupt);
    }
}

TEST_CASE(memory_store_rejects_leaf_hard_links_on_read_and_append) {
    test::ScopedTempDir temp("memory-hardlink");
    const auto existing = agent::memory_event_to_json(first()).dump() + "\n";
    const auto target = temp.write_text("outside.jsonl", existing);
    std::filesystem::create_directory(temp.path() / "memories");
    const auto leaf = temp.path() / "memories/events.jsonl";
    std::error_code error;
    std::filesystem::create_hard_link(target, leaf, error);
    REQUIRE(!error);
    agent::JsonlMemoryStore store(temp.path());
    REQUIRE(!store.read_all().has_value());
    REQUIRE(!store.append(first()).has_value());
    REQUIRE(bytes(target) == existing);
}

TEST_CASE(memory_store_rejects_directory_and_leaf_symlinks_when_supported) {
    for (const auto *location : {"root", "memories", "leaf", "dangling"}) {
        test::ScopedTempDir temp("memory-symlink");
        const auto outside = temp.path() / "outside";
        std::filesystem::create_directory(outside);
        auto root = temp.path() / "runtime";
        std::error_code error;
        if (std::string(location) == "root") {
            std::filesystem::create_directory_symlink(outside, root, error);
        } else {
            std::filesystem::create_directory(root);
            if (std::string(location) == "memories") {
                std::filesystem::create_directory_symlink(outside, root / "memories",
                                                          error);
            } else {
                std::filesystem::create_directory(root / "memories");
                if (std::string(location) == "leaf")
                    temp.write_text("outside/events.jsonl",
                                    agent::memory_event_to_json(first()).dump() + "\n");
                std::filesystem::create_symlink(outside / "events.jsonl",
                                                root / "memories/events.jsonl", error);
            }
        }
        if (error) {
            std::cout << "SKIP symlink creation unavailable: " << location << "\n";
            continue;
        }
        const auto before = bytes(outside / "events.jsonl");
        agent::JsonlMemoryStore store(root);
        REQUIRE(!store.read_all().has_value());
        REQUIRE(!store.append(first()).has_value());
        REQUIRE(bytes(outside / "events.jsonl") == before);
    }
}

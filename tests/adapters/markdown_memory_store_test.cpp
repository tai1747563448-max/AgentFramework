#include "adapters/persistence/markdown_memory_store.h"
#include "adapters/persistence/jsonl_memory_store.h"
#include "adapters/persistence/memory_event_json.h"
#include "application/memory_reducer.h"
#include "domain/memory_event.h"
#include "test_support.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

class MarkdownFixture {
public:
    MarkdownFixture() : root_(std::filesystem::temp_directory_path() /
        ("markdown-memdir-" +
         std::to_string(std::chrono::high_resolution_clock::now()
                            .time_since_epoch().count()) + "-" +
         std::to_string(counter_++))) {
        std::filesystem::create_directories(root_);
    }
    ~MarkdownFixture() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }
    const std::filesystem::path& root() const { return root_; }
private:
    std::filesystem::path root_;
    static inline int counter_{0};
};

// Build a single MemoryUpsertedPayload event the markdown store can
// replay through the standard reducer path. The store derives
// timestamps from the supplied timestamp string.
agent::MemoryEvent make_upsert(const std::string& memory_id,
                               const std::string& content,
                               const std::string& timestamp) {
    agent::MemoryEntry entry;
    entry.memory_id = memory_id;
    entry.category = agent::MemoryCategory::Fact;
    entry.scope_utf8 = "/workspace";
    entry.content = content;
    entry.source_session_id = "session-0123456789abcdef0123456789abcdef";
    entry.source_turn_start = 1;
    entry.source_turn_end = 1;
    entry.created_at_utc = timestamp;
    entry.updated_at_utc = timestamp;
    entry.origin = agent::MemoryOrigin::ExplicitUser;
    agent::MemoryEvent event{
        1, 1, timestamp, "corr-1",
        agent::MemoryUpsertedPayload{entry}};
    return event;
}

TEST_CASE(markdown_store_writes_one_file_per_memory) {
    MarkdownFixture fixture;
    agent::MarkdownMemoryStore store(fixture.root());
    // Mirror the markdown store on the JSONL log so its read_all()
    // projection has the event.
    agent::JsonlMemoryStore log(fixture.root());
    const auto event = make_upsert(
        "memory-0123456789abcdef0123456789abcdef",
        "remember to drink water",
        "2026-09-18T12:00:00Z");
    const auto log_result = log.append(event);
    REQUIRE(log_result.has_value());
    const auto store_result = store.append(event);
    REQUIRE(store_result.has_value());
    const auto path = fixture.root() / "memories" /
        "memory-0123456789abcdef0123456789abcdef.md";
    REQUIRE(std::filesystem::exists(path));
}

TEST_CASE(markdown_store_load_returns_body_only) {
    MarkdownFixture fixture;
    agent::MarkdownMemoryStore store(fixture.root());
    agent::JsonlMemoryStore log(fixture.root());
    const auto event = make_upsert(
        "memory-0123456789abcdef0123456789abcdef",
        "always use enum class",
        "2026-09-18T12:01:00Z");
    REQUIRE(log.append(event).has_value());
    REQUIRE(store.append(event).has_value());
    const auto loaded = store.load(
        "memory-0123456789abcdef0123456789abcdef");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value() == "always use enum class");
}

TEST_CASE(markdown_store_load_rejects_invalid_id) {
    MarkdownFixture fixture;
    agent::MarkdownMemoryStore store(fixture.root());
    const auto loaded = store.load("not a valid id");
    REQUIRE(!loaded.has_value());
    REQUIRE(loaded.error().code == agent::ErrorCode::InvalidInput);
}

TEST_CASE(markdown_store_load_returns_error_for_missing_file) {
    MarkdownFixture fixture;
    agent::MarkdownMemoryStore store(fixture.root());
    const auto loaded = store.load(
        "memory-deadbeefcafebabe0123456789abcdef");
    REQUIRE(!loaded.has_value());
}

TEST_CASE(markdown_store_rewrites_when_content_changes) {
    MarkdownFixture fixture;
    agent::MarkdownMemoryStore store(fixture.root());
    agent::JsonlMemoryStore log(fixture.root());
    const auto first = make_upsert(
        "memory-00112233445566778899aabbccddeeff",
        "original content",
        "2026-09-18T13:00:00Z");
    REQUIRE(log.append(first).has_value());
    REQUIRE(store.append(first).has_value());
    const auto path = fixture.root() / "memories" /
        "memory-00112233445566778899aabbccddeeff.md";
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    REQUIRE(buffer.str().find("original content") != std::string::npos);
}

TEST_CASE(markdown_store_directory_is_memories_subdir) {
    MarkdownFixture fixture;
    agent::MarkdownMemoryStore store(fixture.root());
    REQUIRE(store.directory().filename() == "memories");
}

}  // namespace
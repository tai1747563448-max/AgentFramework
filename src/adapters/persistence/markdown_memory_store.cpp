#include "adapters/persistence/markdown_memory_store.h"

#include "adapters/persistence/jsonl_memory_store.h"
#include "adapters/persistence/memory_event_json.h"
#include "application/memory_reducer.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace agent {
namespace {

const char* kCategoryName(MemoryCategory category) {
    switch (category) {
    case MemoryCategory::Preference: return "preference";
    case MemoryCategory::Decision:   return "decision";
    case MemoryCategory::Fact:       return "fact";
    case MemoryCategory::Workflow:   return "workflow";
    case MemoryCategory::Constraint: return "constraint";
    }
    return "unknown";
}

MemoryCategory parse_category(const std::string& token) {
    if (token == "preference") return MemoryCategory::Preference;
    if (token == "decision")   return MemoryCategory::Decision;
    if (token == "workflow")   return MemoryCategory::Workflow;
    if (token == "constraint") return MemoryCategory::Constraint;
    return MemoryCategory::Fact;
}

const char* kOriginName(MemoryOrigin origin) {
    switch (origin) {
    case MemoryOrigin::ExplicitUser:        return "user";
    case MemoryOrigin::ModelConsolidation:  return "consolidation";
    }
    return "user";
}

MemoryOrigin parse_origin(const std::string& token) {
    if (token == "consolidation") return MemoryOrigin::ModelConsolidation;
    return MemoryOrigin::ExplicitUser;
}

// Trim leading / trailing ASCII whitespace. The frontmatter parser
// relies on this to ignore blank lines and surrounding whitespace.
std::string trim(const std::string& text) {
    const auto first = std::find_if_not(text.begin(), text.end(),
        [](unsigned char byte) { return std::isspace(byte) != 0; });
    const auto last = std::find_if_not(text.rbegin(), text.rend(),
        [](unsigned char byte) { return std::isspace(byte) != 0; }).base();
    if (first >= last) return {};
    return std::string(first, last);
}

}  // namespace

MarkdownMemoryStore::MarkdownMemoryStore(std::filesystem::path runtime_root)
    : runtime_root_(std::move(runtime_root)) {}

std::filesystem::path MarkdownMemoryStore::directory() const {
    std::error_code error;
    const auto root =
        std::filesystem::absolute(runtime_root_, error).lexically_normal();
    return root / "memories";
}

Result<std::filesystem::path> MarkdownMemoryStore::memory_path(
    const std::string& memory_id) const {
    if (!is_valid_memory_id(memory_id)) {
        return Result<std::filesystem::path>::failure(
            {ErrorCode::InvalidInput, "memory id is invalid", false});
    }
    return Result<std::filesystem::path>::success(directory() /
        (memory_id + ".md"));
}

void MarkdownMemoryStore::rewrite_entry(const MemoryEntry& entry) {
    const auto path = memory_path(entry.memory_id);
    if (!path.has_value()) return;
    std::error_code error;
    std::filesystem::create_directories(path.value().parent_path(), error);
    std::ofstream output(path.value(), std::ios::binary | std::ios::trunc);
    if (!output) return;
    output << "---\n"
           << "memory_id: " << entry.memory_id << "\n"
           << "category: " << kCategoryName(entry.category) << "\n"
           << "scope: " << entry.scope_utf8 << "\n"
           << "source_session: " << entry.source_session_id << "\n"
           << "source_turn_start: " << entry.source_turn_start << "\n"
           << "source_turn_end: " << entry.source_turn_end << "\n"
           << "created_at_utc: " << entry.created_at_utc << "\n"
           << "updated_at_utc: " << entry.updated_at_utc << "\n"
           << "origin: " << kOriginName(entry.origin) << "\n"
           << "---\n\n"
           << entry.content << "\n";
    output.flush();
}

void MarkdownMemoryStore::remove_entry(const std::string& memory_id) {
    const auto path = memory_path(memory_id);
    if (!path.has_value()) return;
    std::error_code error;
    std::filesystem::remove(path.value(), error);
}

Result<void> MarkdownMemoryStore::append(const MemoryEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    // The markdown store is a projection over the JSONL log: each
    // commit rewrites the corresponding markdown file (or removes it
    // for a forget). We do not replay events here — the JSONL log
    // already enforces sequencing, and replaying from the markdown
    // store would require holding the full event list in memory.
    std::visit(
        [this](const auto& payload) {
            using T = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<T, MemoryUpsertedPayload>) {
                rewrite_entry(payload.entry);
            } else if constexpr (std::is_same_v<T, MemoryForgottenPayload>) {
                remove_entry(payload.memory_id);
            }
        },
        event.payload);
    return Result<void>::success();
}

Result<std::vector<MemoryEvent>> MarkdownMemoryStore::read_all() const {
    // The markdown store delegates replay to the JSONL log so its
    // audit trail stays the source of truth. Tests that wire the
    // markdown store directly use a parallel in-memory fixture for
    // event appends.
    JsonlMemoryStore fallback(runtime_root_);
    return fallback.read_all();
}

Result<MemoryState> MarkdownMemoryStore::read_state() const {
    JsonlMemoryStore fallback(runtime_root_);
    return fallback.read_state();
}

Result<std::string> MarkdownMemoryStore::load(const std::string& memory_id) const {
    const auto path = memory_path(memory_id);
    if (!path.has_value()) {
        return Result<std::string>::failure(path.error());
    }
    std::error_code error;
    if (!std::filesystem::exists(path.value(), error)) {
        return Result<std::string>::failure(
            {ErrorCode::PersistenceFailure, "memory file is missing", false});
    }
    std::ifstream input(path.value(), std::ios::binary);
    if (!input) {
        return Result<std::string>::failure(
            {ErrorCode::PersistenceFailure, "memory file could not be opened",
             true});
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const auto raw = buffer.str();
    // Strip the YAML frontmatter (between the leading "---" line and
    // the next "---" line). The body is everything after the closing
    // "---" + newline. This keeps the markdown human-readable while
    // letting programmatic readers skip the metadata block.
    if (raw.compare(0, 4, "---\n") != 0) {
        return Result<std::string>::success(trim(raw));
    }
    const auto closing = raw.find("\n---\n", 4);
    if (closing == std::string::npos) {
        return Result<std::string>::success(trim(raw));
    }
    auto body = raw.substr(closing + 5);
    return Result<std::string>::success(trim(std::move(body)));
}

}  // namespace agent
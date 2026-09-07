#include "adapters/persistence/jsonl_session_store.h"
#include "adapters/persistence/session_event_json.h"
#include "application/session_reducer.h"
#include "test_support.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#endif

namespace fixtures {

constexpr const char* kFirstSession =
    "session-11111111111111111111111111111111";
constexpr const char* kSecondSession =
    "session-22222222222222222222222222222222";
constexpr const char* kTaskId =
    "task-11111111111111111111111111111111";

agent::SessionEvent started(const std::string& session_id,
                            const std::string& timestamp,
                            const std::string& workspace = "E:/workspace") {
    return {1, 1, session_id, timestamp, "corr-session-1",
            agent::SessionStartedPayload{workspace, "MiniMax-M3"}};
}

agent::SessionEvent turn_started(const std::string& session_id,
                                 const std::string& timestamp) {
    return {1, 2, session_id, timestamp, "corr-session-2",
            agent::SessionTurnStartedPayload{1, kTaskId, "question"}};
}

std::string read_all(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

std::string duplicate_member(std::string json,
                             const std::string& member) {
    const auto position = json.find(member);
    if (position == std::string::npos) {
        throw std::runtime_error(
            "JSON member fixture was not found: " + member);
    }
    json.insert(position, member + ",");
    return json;
}

bool symlink_creation_lacks_privilege(const std::error_code& error) {
#if defined(_WIN32)
    return error.value() == ERROR_PRIVILEGE_NOT_HELD;
#else
    return error == std::errc::operation_not_permitted ||
           error == std::errc::permission_denied;
#endif
}

#if defined(_WIN32)
struct MountPointReparseData {
    DWORD reparse_tag;
    WORD reparse_data_length;
    WORD reserved;
    WORD substitute_name_offset;
    WORD substitute_name_length;
    WORD print_name_offset;
    WORD print_name_length;
    wchar_t path_buffer[1];
};

bool create_directory_junction(const std::filesystem::path& target,
                               const std::filesystem::path& junction,
                               std::error_code& error) {
    error.clear();
    std::error_code path_error;
    const auto absolute_target =
        std::filesystem::absolute(target, path_error).lexically_normal();
    if (path_error) {
        error = path_error;
        return false;
    }
    if (CreateDirectoryW(junction.c_str(), nullptr) == 0) {
        error = std::error_code(static_cast<int>(GetLastError()),
                                std::system_category());
        return false;
    }
    const HANDLE handle = CreateFileW(
        junction.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = std::error_code(static_cast<int>(GetLastError()),
                                std::system_category());
        RemoveDirectoryW(junction.c_str());
        return false;
    }

    const std::wstring print_name = absolute_target.native();
    const std::wstring substitute_name = L"\\??\\" + print_name;
    const auto substitute_bytes =
        substitute_name.size() * sizeof(wchar_t);
    const auto print_bytes = print_name.size() * sizeof(wchar_t);
    const auto path_bytes = substitute_bytes + sizeof(wchar_t) +
                            print_bytes + sizeof(wchar_t);
    std::vector<std::max_align_t> storage(
        (offsetof(MountPointReparseData, path_buffer) + path_bytes +
         sizeof(std::max_align_t) - 1) /
        sizeof(std::max_align_t));
    auto* data = reinterpret_cast<MountPointReparseData*>(storage.data());
    std::memset(data, 0,
                offsetof(MountPointReparseData, path_buffer) + path_bytes);
    data->reparse_tag = IO_REPARSE_TAG_MOUNT_POINT;
    data->substitute_name_offset = 0;
    data->substitute_name_length =
        static_cast<WORD>(substitute_bytes);
    data->print_name_offset =
        static_cast<WORD>(substitute_bytes + sizeof(wchar_t));
    data->print_name_length = static_cast<WORD>(print_bytes);
    data->reparse_data_length = static_cast<WORD>(
        4 * sizeof(WORD) + path_bytes);
    std::memcpy(data->path_buffer, substitute_name.c_str(),
                substitute_bytes);
    auto* print_destination = reinterpret_cast<unsigned char*>(
        data->path_buffer) + data->print_name_offset;
    std::memcpy(print_destination, print_name.c_str(), print_bytes);

    DWORD returned = 0;
    const BOOL created = DeviceIoControl(
        handle, FSCTL_SET_REPARSE_POINT, data,
        static_cast<DWORD>(offsetof(MountPointReparseData, path_buffer) +
                           path_bytes),
        nullptr, 0, &returned, nullptr);
    const auto last_error = created != 0 ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (created == 0) {
        RemoveDirectoryW(junction.c_str());
        error = std::error_code(static_cast<int>(last_error),
                                std::system_category());
        return false;
    }
    return true;
}
#endif

}  // namespace fixtures

TEST_CASE(jsonl_session_store_appends_and_reloads_real_session_events) {
    test::ScopedTempDir temp("jsonl-session-round-trip");
    agent::JsonlSessionStore store(temp.path());
    const auto first = fixtures::started(
        fixtures::kFirstSession, "2026-09-06T10:00:00.000Z");
    const auto second = fixtures::turn_started(
        fixtures::kFirstSession, "2026-09-06T10:01:00.000Z");

    const auto first_append = store.append(first);
    if (!first_append.has_value()) {
        std::cout << "first append diagnostic: "
                  << first_append.error().message << '\n';
    }
    REQUIRE(first_append.has_value());
    REQUIRE(store.append(second).has_value());
    const auto loaded = store.read_session(fixtures::kFirstSession);

    REQUIRE(loaded.has_value());
    const std::vector<agent::SessionEvent> expected{first, second};
    REQUIRE(loaded.value() == expected);
    const auto path = store.event_path(fixtures::kFirstSession);
    REQUIRE(path.has_value());
    const auto bytes = fixtures::read_all(path.value());
    REQUIRE(!bytes.empty());
    REQUIRE(bytes.back() == '\n');
    REQUIRE(bytes.find("MiniMax-M3") != std::string::npos);
}

TEST_CASE(jsonl_session_store_lists_latest_session_first) {
    test::ScopedTempDir temp("jsonl-session-list");
    agent::JsonlSessionStore store(temp.path());
    REQUIRE(store.append(fixtures::started(
                fixtures::kFirstSession,
                "2026-09-06T10:00:00.000Z", "E:/first"))
                .has_value());
    REQUIRE(store.append(fixtures::started(
                fixtures::kSecondSession,
                "2026-09-06T11:00:00.000Z", "E:/second"))
                .has_value());

    const auto sessions = store.list_sessions();

    REQUIRE(sessions.has_value());
    REQUIRE(sessions.value().size() == 2);
    REQUIRE(sessions.value().at(0).session_id == fixtures::kSecondSession);
    REQUIRE(sessions.value().at(1).session_id == fixtures::kFirstSession);
}

TEST_CASE(jsonl_session_store_rejects_invalid_ids_and_truncated_records) {
    test::ScopedTempDir temp("jsonl-session-invalid");
    agent::JsonlSessionStore store(temp.path());
    REQUIRE(!store.event_path("session-invalid").has_value());
    REQUIRE(!store.read_session("session-invalid").has_value());

    const auto path = store.event_path(fixtures::kFirstSession);
    REQUIRE(path.has_value());
    std::filesystem::create_directories(path.value().parent_path());
    temp.write_text(
        std::filesystem::path("sessions") / fixtures::kFirstSession /
            "events.jsonl",
        "{\"schema_version\":1}");

    REQUIRE(!store.read_session(fixtures::kFirstSession).has_value());
    REQUIRE(!store.list_sessions().has_value());
}

TEST_CASE(jsonl_session_store_rejects_session_directory_symlink_when_supported) {
    test::ScopedTempDir temp("jsonl-session-link");
    const auto outside = temp.path() / "outside";
    std::filesystem::create_directory(outside);
    const auto sessions = temp.path() / "sessions";
    std::filesystem::create_directory(sessions);
    const auto linked = sessions / fixtures::kFirstSession;
    std::error_code error;
    std::filesystem::create_directory_symlink(outside, linked, error);
    if (error) {
        REQUIRE(fixtures::symlink_creation_lacks_privilege(error));
        return;
    }

    agent::JsonlSessionStore store(temp.path());
    const auto result = store.append(fixtures::started(
        fixtures::kFirstSession, "2026-09-06T10:00:00.000Z"));

    REQUIRE(!result.has_value());
    REQUIRE(!std::filesystem::exists(outside / "events.jsonl"));
}

TEST_CASE(jsonl_session_store_rejects_duplicate_json_object_keys) {
    test::ScopedTempDir temp("jsonl-session-duplicate-key");
    agent::JsonlSessionStore store(temp.path());
    const auto path = store.event_path(fixtures::kFirstSession);
    REQUIRE(path.has_value());
    temp.write_text(
        std::filesystem::path("sessions") / fixtures::kFirstSession /
            "events.jsonl",
        "{\"schema_version\":1,\"schema_version\":1,"
        "\"sequence\":1,"
        "\"session_id\":\"session-11111111111111111111111111111111\","
        "\"timestamp\":\"2026-09-06T10:00:00.000Z\","
        "\"correlation_id\":\"corr-session-1\","
        "\"event_type\":\"session_started\","
        "\"payload\":{\"workspace_utf8\":\"E:/workspace\","
        "\"model\":\"MiniMax-M3\"}}\n");

    REQUIRE(!store.read_session(fixtures::kFirstSession).has_value());

    const auto started = agent::session_event_to_json(fixtures::started(
                             fixtures::kFirstSession,
                             "2026-09-06T10:00:00.000Z"))
                             .dump();
    const auto turn = agent::session_event_to_json(fixtures::turn_started(
                          fixtures::kFirstSession,
                          "2026-09-06T10:01:00.000Z"))
                          .dump();
    const agent::ToolCall call{
        "call-1", "read_file",
        agent::Value::object({{"path", agent::Value("notes.txt")}})};
    const std::vector<agent::Message> messages{
        {agent::Role::User, {agent::TextBlock{"question"}}},
        {agent::Role::Assistant,
         {agent::TextBlock{"checking"}, agent::ToolUseBlock{call}}},
        {agent::Role::User,
         {agent::ToolResultBlock{{"call-1", "contents", false}}}},
        {agent::Role::Assistant, {agent::TextBlock{"answer"}}}};
    const auto committed = agent::session_event_to_json(
                               {1, 3, fixtures::kFirstSession,
                                "2026-09-06T10:02:00.000Z", "corr-session-3",
                                agent::SessionTurnCommittedPayload{
                                    1, fixtures::kTaskId, messages}})
                               .dump();
    for (const std::string member : {
             "\"model\":\"MiniMax-M3\"",
             "\"role\":\"assistant\"",
             "\"text\":\"checking\"",
             "\"id\":\"call-1\"",
             "\"tool_call_id\":\"call-1\""}) {
        const bool started_member = member.find("model") != std::string::npos;
        const auto corrupted = started_member
                                   ? fixtures::duplicate_member(started, member) +
                                         "\n"
                                   : started + "\n" + turn + "\n" +
                                         fixtures::duplicate_member(
                                             committed, member) +
                                         "\n";
        temp.write_text(
            std::filesystem::path("sessions") / fixtures::kFirstSession /
                "events.jsonl",
            corrupted);
        REQUIRE(!store.read_session(fixtures::kFirstSession).has_value());
    }
}

TEST_CASE(jsonl_session_store_rejects_leaf_hard_link_without_modifying_target) {
    test::ScopedTempDir temp("jsonl-session-hard-link");
    const auto external = temp.path() / "external.txt";
    const auto existing = agent::session_event_to_json(fixtures::started(
                              fixtures::kFirstSession,
                              "2026-09-06T10:00:00.000Z"))
                              .dump() +
                          "\n";
    temp.write_text("external.txt", existing);
    const auto leaf =
        temp.path() / "sessions" / fixtures::kFirstSession / "events.jsonl";
    std::filesystem::create_directories(leaf.parent_path());
    std::error_code error;
    std::filesystem::create_hard_link(external, leaf, error);
    if (error) {
        return;
    }
    agent::JsonlSessionStore store(temp.path());

    const auto result = store.append(fixtures::turn_started(
        fixtures::kFirstSession, "2026-09-06T10:01:00.000Z"));

    REQUIRE(!result.has_value());
    REQUIRE(fixtures::read_all(external) == existing);
}

TEST_CASE(jsonl_session_store_read_rejects_leaf_hard_link) {
    test::ScopedTempDir temp("jsonl-session-read-hard-link");
    const auto external = temp.path() / "external.txt";
    const auto existing = agent::session_event_to_json(fixtures::started(
                              fixtures::kFirstSession,
                              "2026-09-06T10:00:00.000Z"))
                              .dump() +
                          "\n";
    temp.write_text("external.txt", existing);
    const auto leaf =
        temp.path() / "sessions" / fixtures::kFirstSession / "events.jsonl";
    std::filesystem::create_directories(leaf.parent_path());
    std::error_code error;
    std::filesystem::create_hard_link(external, leaf, error);
    if (error) {
        return;
    }

    agent::JsonlSessionStore store(temp.path());

    REQUIRE(!store.read_session(fixtures::kFirstSession).has_value());
}

TEST_CASE(jsonl_session_store_rejects_sessions_root_symlink_when_supported) {
    test::ScopedTempDir temp("jsonl-session-root-link");
    const auto outside = temp.path() / "outside";
    std::filesystem::create_directory(outside);
    std::error_code error;
    std::filesystem::create_directory_symlink(
        outside, temp.path() / "sessions", error);
    if (error) {
        REQUIRE(fixtures::symlink_creation_lacks_privilege(error));
        return;
    }
    agent::JsonlSessionStore store(temp.path());

    const auto result = store.append(fixtures::started(
        fixtures::kFirstSession, "2026-09-06T10:00:00.000Z"));

    REQUIRE(!result.has_value());
    REQUIRE(!std::filesystem::exists(
        outside / fixtures::kFirstSession / "events.jsonl"));
}

TEST_CASE(jsonl_session_store_rejects_dangling_leaf_symlink_when_supported) {
    test::ScopedTempDir temp("jsonl-session-dangling-link");
    const auto outside = temp.path() / "outside";
    std::filesystem::create_directory(outside);
    const auto leaf =
        temp.path() / "sessions" / fixtures::kFirstSession / "events.jsonl";
    std::filesystem::create_directories(leaf.parent_path());
    const auto target = outside / "created-by-link.jsonl";
    std::error_code error;
    std::filesystem::create_symlink(target, leaf, error);
    if (error) {
        REQUIRE(fixtures::symlink_creation_lacks_privilege(error));
        return;
    }
    agent::JsonlSessionStore store(temp.path());

    const auto result = store.append(fixtures::started(
        fixtures::kFirstSession, "2026-09-06T10:00:00.000Z"));

    REQUIRE(!result.has_value());
    REQUIRE(!std::filesystem::exists(target));
}

TEST_CASE(jsonl_session_store_serializes_competing_next_sequence_writers) {
    test::ScopedTempDir temp("jsonl-session-competing-writers");
    agent::JsonlSessionStore seed_store(temp.path());
    REQUIRE(seed_store
                .append(fixtures::started(
                    fixtures::kFirstSession,
                    "2026-09-06T10:00:00.000Z"))
                .has_value());

    constexpr std::size_t kWriterCount = 8;
    std::atomic<std::size_t> ready{0};
    std::atomic<bool> start{false};
    std::vector<int> accepted(kWriterCount, 0);
    std::vector<std::thread> writers;
    writers.reserve(kWriterCount);
    for (std::size_t index = 0; index < kWriterCount; ++index) {
        writers.emplace_back([&, index] {
            agent::JsonlSessionStore store(temp.path());
            agent::SessionEvent event{
                1,
                2,
                fixtures::kFirstSession,
                "2026-09-06T10:01:00.000Z",
                "corr-competing-" + std::to_string(index),
                agent::SessionTurnStartedPayload{
                    1,
                    fixtures::kTaskId,
                    std::string(index * 1024 * 1024, 'a') +
                        std::to_string(index)}};
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            accepted[index] = store.append(event).has_value() ? 1 : 0;
        });
    }
    while (ready.load(std::memory_order_acquire) != kWriterCount) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (auto& writer : writers) {
        writer.join();
    }

    int accepted_count = 0;
    for (const auto value : accepted) {
        accepted_count += value;
    }
    REQUIRE(accepted_count == 1);
    const auto loaded = seed_store.read_session(fixtures::kFirstSession);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded.value().size() == 2);
    REQUIRE(loaded.value().back().sequence == 2);
}

TEST_CASE(jsonl_session_store_rejects_existing_runtime_ancestor_symlink) {
    test::ScopedTempDir temp("jsonl-session-ancestor-link");
    const auto outside = temp.path() / "outside";
    const auto runtime = outside / "runtime";
    std::filesystem::create_directories(runtime);
    const auto linked_ancestor = temp.path() / "linked-ancestor";
    std::error_code error;
    std::filesystem::create_directory_symlink(outside, linked_ancestor, error);
    if (error) {
        REQUIRE(fixtures::symlink_creation_lacks_privilege(error));
        return;
    }
    agent::JsonlSessionStore store(linked_ancestor / "runtime");

    const auto result = store.append(fixtures::started(
        fixtures::kFirstSession, "2026-09-06T10:00:00.000Z"));

    REQUIRE(!result.has_value());
    REQUIRE(!std::filesystem::exists(
        runtime / "sessions" / fixtures::kFirstSession / "events.jsonl"));
    REQUIRE(!store.list_sessions().has_value());
}

TEST_CASE(jsonl_session_store_rejects_missing_runtime_under_ancestor_symlink) {
    test::ScopedTempDir temp("jsonl-session-missing-root-link");
    const auto outside = temp.path() / "outside";
    std::filesystem::create_directory(outside);
    const auto linked_ancestor = temp.path() / "linked-ancestor";
    std::error_code error;
    std::filesystem::create_directory_symlink(outside, linked_ancestor, error);
    if (error) {
        REQUIRE(fixtures::symlink_creation_lacks_privilege(error));
        return;
    }
    const auto runtime = linked_ancestor / "missing-runtime";
    agent::JsonlSessionStore store(runtime);

    const auto result = store.append(fixtures::started(
        fixtures::kFirstSession, "2026-09-06T10:00:00.000Z"));

    REQUIRE(!result.has_value());
    REQUIRE(!std::filesystem::exists(outside / "missing-runtime"));
    REQUIRE(!store.list_sessions().has_value());
}

#if defined(_WIN32)
TEST_CASE(jsonl_session_store_rejects_runtime_ancestor_junction) {
    test::ScopedTempDir temp("jsonl-session-ancestor-junction");
    const auto outside = temp.path() / "outside";
    const auto runtime = outside / "runtime";
    std::filesystem::create_directories(runtime);
    const auto junction = temp.path() / "ancestor-junction";
    std::error_code error;
    REQUIRE(fixtures::create_directory_junction(outside, junction, error));
    agent::JsonlSessionStore store(junction / "runtime");

    const auto result = store.append(fixtures::started(
        fixtures::kFirstSession, "2026-09-06T10:00:00.000Z"));

    REQUIRE(!result.has_value());
    REQUIRE(!std::filesystem::exists(
        runtime / "sessions" / fixtures::kFirstSession / "events.jsonl"));
    REQUIRE(!store.list_sessions().has_value());
}

TEST_CASE(jsonl_session_store_rejects_missing_runtime_under_ancestor_junction) {
    test::ScopedTempDir temp("jsonl-session-missing-root-junction");
    const auto outside = temp.path() / "outside";
    std::filesystem::create_directory(outside);
    const auto junction = temp.path() / "ancestor-junction";
    std::error_code error;
    REQUIRE(fixtures::create_directory_junction(outside, junction, error));
    agent::JsonlSessionStore store(junction / "missing-runtime");

    const auto result = store.append(fixtures::started(
        fixtures::kFirstSession, "2026-09-06T10:00:00.000Z"));

    REQUIRE(!result.has_value());
    REQUIRE(!std::filesystem::exists(outside / "missing-runtime"));
    REQUIRE(!store.list_sessions().has_value());
}
#endif

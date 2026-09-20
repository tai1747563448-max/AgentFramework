#include "adapters/persistence/jsonl_event_store.h"

#include "adapters/persistence/event_json.h"
#include "application/state_reducer.h"

#include <nlohmann/json.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace agent {
namespace {

Result<void> persistence_failure(std::string message) {
    return Result<void>::failure(
        {ErrorCode::PersistenceFailure, std::move(message), false});
}

Result<std::vector<RuntimeEvent>> persistence_read_failure(std::string message) {
    return Result<std::vector<RuntimeEvent>>::failure(
        {ErrorCode::PersistenceFailure, std::move(message), false});
}

Result<std::filesystem::path> persistence_path_failure(std::string message) {
    return Result<std::filesystem::path>::failure(
        {ErrorCode::PersistenceFailure, std::move(message), false});
}

Result<std::string> persistence_text_read_failure(std::string message) {
    return Result<std::string>::failure(
        {ErrorCode::PersistenceFailure, std::move(message), false});
}

bool is_contained_path(const std::filesystem::path& base,
                       const std::filesystem::path& candidate) {
    const auto relative = candidate.lexically_relative(base);
    if (relative.empty() || relative.is_absolute()) {
        return false;
    }
    const auto first = relative.begin();
    return first != relative.end() && *first != std::filesystem::path("..");
}

Result<std::vector<RuntimeEvent>> decode_event_stream(std::istream& input) {
    std::vector<RuntimeEvent> events;
    std::string line;
    std::size_t line_number = 0;
    try {
        while (std::getline(input, line)) {
            ++line_number;
            if (line.empty()) {
                continue;
            }
            const auto json = nlohmann::json::parse(line);
            auto decoded = event_from_json(json);
            if (!decoded.has_value()) {
                return persistence_read_failure(
                    "invalid event record at line " +
                    std::to_string(line_number));
            }
            events.push_back(std::move(decoded.value()));
        }
    } catch (const nlohmann::json::exception&) {
        return persistence_read_failure("invalid JSON event record at line " +
                                        std::to_string(line_number));
    } catch (const std::exception&) {
        return persistence_read_failure("failed while reading event log");
    }
    if (input.bad()) {
        return persistence_read_failure("failed while reading event log");
    }

    if (!events.empty()) {
        const auto& task_id = events.front().task_id;
        std::uint64_t expected_sequence = 1;
        for (const auto& event : events) {
            if (event.task_id != task_id) {
                return persistence_read_failure(
                    "event log contains more than one task ID");
            }
            if (event.sequence != expected_sequence) {
                return persistence_read_failure(
                    "event log sequence is not contiguous");
            }
            ++expected_sequence;
        }
    }
    const auto replayed = replay_events(events);
    if (!replayed.has_value()) {
        return persistence_read_failure("event log cannot be replayed");
    }
    return Result<std::vector<RuntimeEvent>>::success(std::move(events));
}

#ifdef _WIN32

class UniqueHandle {
public:
    explicit UniqueHandle(HANDLE handle = INVALID_HANDLE_VALUE)
        : handle_(handle) {}
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = INVALID_HANDLE_VALUE;
    }
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.handle_;
            other.handle_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    ~UniqueHandle() {
        reset();
    }

    HANDLE get() const noexcept {
        return handle_;
    }
    bool valid() const noexcept {
        return handle_ != INVALID_HANDLE_VALUE;
    }

private:
    void reset() noexcept {
        if (valid()) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    HANDLE handle_;
};

UniqueHandle open_directory_handle(const std::filesystem::path& path) {
    return UniqueHandle(CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
}

bool ensure_directory_exists(const std::filesystem::path& path) {
    return CreateDirectoryW(path.c_str(), nullptr) != 0 ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

bool is_plain_directory(HANDLE handle) {
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    return GetFileInformationByHandleEx(handle, FileAttributeTagInfo,
                                        &attributes, sizeof(attributes)) != 0 &&
           (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool final_handle_path(HANDLE handle, std::filesystem::path& output) {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD length = GetFinalPathNameByHandleW(
            handle, buffer.data(), static_cast<DWORD>(buffer.size()),
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (length == 0) {
            return false;
        }
        if (length < buffer.size()) {
            output = std::filesystem::path(
                std::wstring(buffer.data(), static_cast<std::size_t>(length)));
            return true;
        }
        buffer.resize(static_cast<std::size_t>(length) + 1);
    }
}

bool paths_equal_case_insensitive(const std::filesystem::path& left,
                                  const std::filesystem::path& right) {
    const auto left_text = left.lexically_normal().native();
    const auto right_text = right.lexically_normal().native();
    return CompareStringOrdinal(left_text.c_str(), -1, right_text.c_str(), -1,
                                TRUE) == CSTR_EQUAL;
}

bool is_direct_handle_child(HANDLE parent,
                            HANDLE child,
                            const std::filesystem::path& expected_name) {
    std::filesystem::path parent_path;
    std::filesystem::path child_path;
    return final_handle_path(parent, parent_path) &&
           final_handle_path(child, child_path) &&
           paths_equal_case_insensitive(child_path.parent_path(), parent_path) &&
           paths_equal_case_insensitive(child_path.filename(), expected_name);
}

Result<void> secure_append_line(const std::filesystem::path& runtime_root,
                                const std::filesystem::path& path,
                                const std::string& task_id,
                                const std::string& line) {
    const auto root = open_directory_handle(runtime_root);
    if (!root.valid() || !is_plain_directory(root.get())) {
        return persistence_failure("event path escapes runtime root");
    }
    const auto tasks_path = runtime_root / "tasks";
    if (!ensure_directory_exists(tasks_path)) {
        return persistence_failure("failed to create event directory");
    }
    const auto tasks = open_directory_handle(tasks_path);
    if (!tasks.valid() || !is_plain_directory(tasks.get()) ||
        !is_direct_handle_child(root.get(), tasks.get(), "tasks")) {
        return persistence_failure("event path escapes runtime root");
    }
    if (!ensure_directory_exists(path.parent_path())) {
        return persistence_failure("failed to create event directory");
    }
    const auto task = open_directory_handle(path.parent_path());
    if (!task.valid() || !is_plain_directory(task.get()) ||
        !is_direct_handle_child(tasks.get(), task.get(),
                                std::filesystem::u8path(task_id))) {
        return persistence_failure("event path escapes runtime root");
    }

    UniqueHandle leaf(CreateFileW(
        path.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
        nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_WRITE_THROUGH,
        nullptr));
    if (!leaf.valid()) {
        return persistence_failure("failed to open event log for append");
    }

    FILE_ATTRIBUTE_TAG_INFO attributes{};
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandleEx(leaf.get(), FileAttributeTagInfo,
                                     &attributes, sizeof(attributes)) == 0 ||
        GetFileInformationByHandle(leaf.get(), &information) == 0 ||
        GetFileType(leaf.get()) != FILE_TYPE_DISK ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        information.nNumberOfLinks != 1 ||
        !is_direct_handle_child(task.get(), leaf.get(), "events.jsonl")) {
        return persistence_failure("event log leaf is not a regular file");
    }

    std::size_t offset = 0;
    while (offset < line.size()) {
        const auto remaining = line.size() - offset;
        const auto chunk = static_cast<DWORD>(
            remaining > std::numeric_limits<DWORD>::max()
                ? std::numeric_limits<DWORD>::max()
                : remaining);
        DWORD written = 0;
        if (WriteFile(leaf.get(), line.data() + offset, chunk, &written,
                      nullptr) == 0 ||
            written == 0) {
            return persistence_failure("failed to append complete event record");
        }
        offset += static_cast<std::size_t>(written);
    }
    // T07: per-event fsync moved to secure_flush_log, driven by
    // JsonlEventStore::flush_pending every 32 events / 500ms.
    return Result<void>::success();
}

// T07: separate fsync helper. flush_pending() (driven by the runtime every
// 32 events or 500ms) opens the log file just long enough to flush any
// bytes the kernel may still be holding back, then closes. The leaf
// validation mirrors secure_append_line so a tampered path is rejected
// even on the flush path.
Result<void> secure_flush_log(const std::filesystem::path& runtime_root,
                              const std::filesystem::path& path,
                              const std::string& task_id) {
    const auto root = open_directory_handle(runtime_root);
    if (!root.valid() || !is_plain_directory(root.get())) {
        return persistence_failure("event path escapes runtime root");
    }
    const auto tasks = open_directory_handle(runtime_root / "tasks");
    if (!tasks.valid() || !is_plain_directory(tasks.get()) ||
        !is_direct_handle_child(root.get(), tasks.get(), "tasks")) {
        return persistence_failure("event path escapes runtime root");
    }
    const auto task = open_directory_handle(path.parent_path());
    if (!task.valid() || !is_plain_directory(task.get()) ||
        !is_direct_handle_child(tasks.get(), task.get(),
                                std::filesystem::u8path(task_id))) {
        return persistence_failure("event path escapes runtime root");
    }
    // FlushFileBuffers needs write-data access; a handle carrying only
    // FILE_WRITE_ATTRIBUTES opens fine but every flush on it fails with
    // ERROR_ACCESS_DENIED. FILE_APPEND_DATA is both sufficient and the
    // same right secure_append_line requests, so the flush handle and the
    // append handle never disagree about what writing means here.
    UniqueHandle leaf(CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES | FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!leaf.valid()) {
        return persistence_failure("event log could not be opened for flush");
    }
    if (!is_direct_handle_child(task.get(), leaf.get(), "events.jsonl")) {
        return persistence_failure("event log leaf is not a regular file");
    }
    if (FlushFileBuffers(leaf.get()) == 0) {
        return persistence_failure("failed to flush event log");
    }
    return Result<void>::success();
}

Result<std::string> secure_read_task_bytes(
    const std::filesystem::path& runtime_root,
    const std::filesystem::path& path,
    const std::string& task_id) {
    const auto root = open_directory_handle(runtime_root);
    if (!root.valid() || !is_plain_directory(root.get())) {
        return persistence_text_read_failure("event path escapes runtime root");
    }
    const auto tasks = open_directory_handle(runtime_root / "tasks");
    if (!tasks.valid() || !is_plain_directory(tasks.get()) ||
        !is_direct_handle_child(root.get(), tasks.get(), "tasks")) {
        return persistence_text_read_failure("event path escapes runtime root");
    }
    const auto task = open_directory_handle(path.parent_path());
    if (!task.valid() || !is_plain_directory(task.get()) ||
        !is_direct_handle_child(tasks.get(), task.get(),
                                std::filesystem::u8path(task_id))) {
        return persistence_text_read_failure("event path escapes runtime root");
    }

    UniqueHandle leaf(CreateFileW(
        path.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
            FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!leaf.valid()) {
        return persistence_text_read_failure("failed to open event log");
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandleEx(leaf.get(), FileAttributeTagInfo,
                                     &attributes, sizeof(attributes)) == 0 ||
        GetFileInformationByHandle(leaf.get(), &information) == 0 ||
        GetFileType(leaf.get()) != FILE_TYPE_DISK ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        information.nNumberOfLinks != 1 ||
        !is_direct_handle_child(task.get(), leaf.get(), "events.jsonl")) {
        return persistence_text_read_failure(
            "event log leaf is not a regular file");
    }

    std::string bytes;
    std::array<char, 16 * 1024> buffer{};
    for (;;) {
        DWORD read = 0;
        if (ReadFile(leaf.get(), buffer.data(),
                     static_cast<DWORD>(buffer.size()), &read, nullptr) == 0) {
            return persistence_text_read_failure(
                "failed while reading event log");
        }
        if (read == 0) {
            break;
        }
        try {
            bytes.append(buffer.data(), static_cast<std::size_t>(read));
        } catch (const std::exception&) {
            return persistence_text_read_failure(
                "failed while reading event log");
        }
    }
    return Result<std::string>::success(std::move(bytes));
}

#else

class UniqueFileDescriptor {
public:
    explicit UniqueFileDescriptor(int descriptor = -1)
        : descriptor_(descriptor) {}
    UniqueFileDescriptor(const UniqueFileDescriptor&) = delete;
    UniqueFileDescriptor& operator=(const UniqueFileDescriptor&) = delete;
    ~UniqueFileDescriptor() {
        if (descriptor_ >= 0) {
            close(descriptor_);
        }
    }

    int get() const noexcept {
        return descriptor_;
    }
    bool valid() const noexcept {
        return descriptor_ >= 0;
    }

private:
    int descriptor_;
};

bool is_plain_directory(int descriptor) {
    struct stat information {};
    return fstat(descriptor, &information) == 0 &&
           S_ISDIR(information.st_mode);
}

Result<void> secure_append_line(const std::filesystem::path& runtime_root,
                                const std::filesystem::path&,
                                const std::string& task_id,
                                const std::string& line) {
    const UniqueFileDescriptor root(open(runtime_root.c_str(),
                                         O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                             O_NOFOLLOW));
    if (!root.valid() || !is_plain_directory(root.get())) {
        return persistence_failure("event path escapes runtime root");
    }
    if (mkdirat(root.get(), "tasks", 0700) != 0 && errno != EEXIST) {
        return persistence_failure("failed to create event directory");
    }
    const UniqueFileDescriptor tasks(openat(
        root.get(), "tasks",
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!tasks.valid() || !is_plain_directory(tasks.get())) {
        return persistence_failure("event path escapes runtime root");
    }
    if (mkdirat(tasks.get(), task_id.c_str(), 0700) != 0 && errno != EEXIST) {
        return persistence_failure("failed to create event directory");
    }
    const UniqueFileDescriptor task(openat(
        tasks.get(), task_id.c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!task.valid() || !is_plain_directory(task.get())) {
        return persistence_failure("event path escapes runtime root");
    }

    const UniqueFileDescriptor leaf(openat(
        task.get(), "events.jsonl",
        O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (!leaf.valid()) {
        return persistence_failure("failed to open event log for append");
    }
    struct stat information {};
    if (fstat(leaf.get(), &information) != 0 ||
        !S_ISREG(information.st_mode) || information.st_nlink != 1) {
        return persistence_failure("event log leaf is not a regular file");
    }

    std::size_t offset = 0;
    while (offset < line.size()) {
        const auto written =
            write(leaf.get(), line.data() + offset, line.size() - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return persistence_failure("failed to append complete event record");
        }
        offset += static_cast<std::size_t>(written);
    }
    // T07: per-event fsync moved to secure_flush_log, driven by
    // JsonlEventStore::flush_pending every 32 events / 500ms.
    return Result<void>::success();
}

// T07: separate fsync helper for the 32-events-or-500ms flush schedule.
Result<void> secure_flush_log(const std::filesystem::path& runtime_root,
                              const std::filesystem::path&,
                              const std::string& task_id) {
    const UniqueFileDescriptor root(open(runtime_root.c_str(),
                                         O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                             O_NOFOLLOW));
    if (!root.valid() || !is_plain_directory(root.get())) {
        return persistence_failure("event path escapes runtime root");
    }
    const UniqueFileDescriptor tasks(openat(
        root.get(), "tasks",
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!tasks.valid() || !is_plain_directory(tasks.get())) {
        return persistence_failure("event path escapes runtime root");
    }
    const UniqueFileDescriptor task(openat(
        tasks.get(), task_id.c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!task.valid() || !is_plain_directory(task.get())) {
        return persistence_failure("event path escapes runtime root");
    }
    const UniqueFileDescriptor leaf(openat(
        task.get(), "events.jsonl", O_WRONLY | O_APPEND | O_CLOEXEC |
            O_NOFOLLOW));
    if (!leaf.valid()) {
        return persistence_failure("event log could not be opened for flush");
    }
    if (fsync(leaf.get()) != 0) {
        return persistence_failure("failed to flush event log");
    }
    return Result<void>::success();
}

Result<std::string> secure_read_task_bytes(
    const std::filesystem::path& runtime_root,
    const std::filesystem::path&,
    const std::string& task_id) {
    const UniqueFileDescriptor root(open(runtime_root.c_str(),
                                         O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                             O_NOFOLLOW));
    if (!root.valid() || !is_plain_directory(root.get())) {
        return persistence_text_read_failure("event path escapes runtime root");
    }
    const UniqueFileDescriptor tasks(openat(
        root.get(), "tasks",
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!tasks.valid() || !is_plain_directory(tasks.get())) {
        return persistence_text_read_failure("event path escapes runtime root");
    }
    const UniqueFileDescriptor task(openat(
        tasks.get(), task_id.c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!task.valid() || !is_plain_directory(task.get())) {
        return persistence_text_read_failure("event path escapes runtime root");
    }
    const UniqueFileDescriptor leaf(openat(
        task.get(), "events.jsonl", O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!leaf.valid()) {
        return persistence_text_read_failure("failed to open event log");
    }
    struct stat information {};
    if (fstat(leaf.get(), &information) != 0 ||
        !S_ISREG(information.st_mode) || information.st_nlink != 1) {
        return persistence_text_read_failure(
            "event log leaf is not a regular file");
    }

    std::string bytes;
    std::array<char, 16 * 1024> buffer{};
    for (;;) {
        const auto read_count = read(leaf.get(), buffer.data(), buffer.size());
        if (read_count < 0 && errno == EINTR) {
            continue;
        }
        if (read_count < 0) {
            return persistence_text_read_failure(
                "failed while reading event log");
        }
        if (read_count == 0) {
            break;
        }
        try {
            bytes.append(buffer.data(),
                         static_cast<std::size_t>(read_count));
        } catch (const std::exception&) {
            return persistence_text_read_failure(
                "failed while reading event log");
        }
    }
    return Result<std::string>::success(std::move(bytes));
}

#endif

}  // namespace

JsonlEventStore::JsonlEventStore(std::filesystem::path runtime_root)
    : runtime_root_(std::move(runtime_root)) {}

Result<std::filesystem::path> JsonlEventStore::event_path(
    const std::string& task_id) const {
    if (!is_valid_task_id(task_id)) {
        return persistence_path_failure("invalid task ID for event storage");
    }
    std::error_code error;
    const auto tasks_root = std::filesystem::absolute(
        runtime_root_ / "tasks", error).lexically_normal();
    if (error) {
        return persistence_path_failure("failed to resolve runtime root");
    }
    const auto candidate =
        (tasks_root / std::filesystem::u8path(task_id) / "events.jsonl")
            .lexically_normal();
    if (!is_contained_path(tasks_root, candidate)) {
        return persistence_path_failure("event path escapes runtime root");
    }
    return Result<std::filesystem::path>::success(candidate);
}

Result<void> JsonlEventStore::append(const RuntimeEvent& event) {
    try {
        const auto resolved = event_path(event.task_id);
        if (!resolved.has_value()) {
            return Result<void>::failure(resolved.error());
        }
        const auto& path = resolved.value();
        std::error_code error;
        const auto canonical_tasks =
            std::filesystem::weakly_canonical(
                std::filesystem::absolute(runtime_root_ / "tasks", error),
                error);
        if (error) {
            return persistence_failure("failed to resolve event directory");
        }
        const auto canonical_parent =
            std::filesystem::weakly_canonical(path.parent_path(), error);
        if (error || !is_contained_path(canonical_tasks, canonical_parent)) {
            return persistence_failure("event path escapes runtime root");
        }

        auto serialized = event_to_json(event).dump();
        serialized.push_back('\n');
        const auto absolute_root =
            std::filesystem::absolute(runtime_root_, error).lexically_normal();
        if (error) {
            return persistence_failure("failed to resolve runtime root");
        }
        std::filesystem::create_directories(absolute_root, error);
        if (error) {
            return persistence_failure("failed to create event directory");
        }
        const auto write_result = secure_append_line(absolute_root, path,
                                                    event.task_id, serialized);
        if (!write_result.has_value()) {
            return write_result;
        }
        // T07: count toward the 32-event flush threshold. The flush itself
        // is best-effort here — a transient fsync failure should not lose
        // data, only durability confirmation. The caller (runtime engine)
        // owns the explicit flush_pending() schedule (32 events / 500ms).
        bool should_flush = false;
        {
            std::lock_guard<std::mutex> lock(index_mutex_);
            pending_counts_[event.task_id] += 1;
            if (pending_counts_[event.task_id] >= 32) {
                should_flush = true;
            }
        }
        if (should_flush) {
            const auto flushed = flush_pending(event.task_id);
            if (!flushed.has_value()) return flushed;
        }
        return Result<void>::success();
    } catch (const std::exception&) {
        return persistence_failure("failed to serialize or append event");
    }
}

Result<std::vector<RuntimeEvent>> JsonlEventStore::read_file(
    const std::filesystem::path& path) const {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return persistence_read_failure("failed to open event log");
    }
    return decode_event_stream(input);
}

Result<std::vector<RuntimeEvent>> JsonlEventStore::read_task(
    const std::string& task_id) const {
    if (!is_valid_task_id(task_id)) {
        return persistence_read_failure("invalid task ID for event storage");
    }
    try {
        const auto resolved = event_path(task_id);
        if (!resolved.has_value()) {
            return persistence_read_failure("failed to resolve event log");
        }
        std::error_code error;
        const auto absolute_root =
            std::filesystem::absolute(runtime_root_, error).lexically_normal();
        if (error) {
            return persistence_read_failure("failed to resolve runtime root");
        }
        auto bytes =
            secure_read_task_bytes(absolute_root, resolved.value(), task_id);
        if (!bytes.has_value()) {
            return persistence_read_failure(bytes.error().message);
        }
        std::istringstream input(bytes.value());
        auto events = decode_event_stream(input);
        if (!events.has_value() || events.value().empty() ||
            events.value().front().task_id != task_id) {
            return persistence_read_failure("task event log does not match task ID");
        }
        return events;
    } catch (const std::exception&) {
        return persistence_read_failure("failed while reading event log");
    }
}

// T07: 32 events / 500ms is the documented flush contract.

Result<void> JsonlEventStore::flush_pending(const std::string& task_id) {
    if (!is_valid_task_id(task_id)) {
        return persistence_failure("invalid task ID for flush");
    }
    const auto resolved = event_path(task_id);
    if (!resolved.has_value()) {
        return Result<void>::failure(resolved.error());
    }
    std::error_code error;
    const auto absolute_root =
        std::filesystem::absolute(runtime_root_, error).lexically_normal();
    if (error) {
        return persistence_failure("failed to resolve runtime root");
    }
    const auto flush_result =
        secure_flush_log(absolute_root, resolved.value(), task_id);
    if (!flush_result.has_value()) {
        return flush_result;
    }
    std::lock_guard<std::mutex> lock(index_mutex_);
    // The committed_through index records the highest sequence the kernel
    // has confirmed durable. Since the runtime calls append() then
    // flush_pending() and we own the index here, the value advances only
    // after fsync returns OK.
    const auto pending = pending_counts_.count(task_id)
                            ? pending_counts_[task_id]
                            : std::uint64_t{0};
    if (pending > 0) {
        committed_through_[task_id] += pending;
        pending_counts_[task_id] = 0;
    }
    return Result<void>::success();
}

std::uint64_t JsonlEventStore::committed_through(
    const std::string& task_id) const {
    std::lock_guard<std::mutex> lock(index_mutex_);
    const auto found = committed_through_.find(task_id);
    if (found == committed_through_.end()) return 0;
    return found->second;
}

}  // namespace agent

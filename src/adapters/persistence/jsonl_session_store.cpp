#include "adapters/persistence/jsonl_session_store.h"

#include "adapters/persistence/session_event_json.h"
#include "application/session_reducer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace agent {
namespace {

template <typename T>
Result<T> persistence_error(const char* message) {
    return Result<T>::failure(
        {ErrorCode::PersistenceFailure, message, false});
}

Result<void> persistence_void_error(const char* message) {
    return Result<void>::failure(
        {ErrorCode::PersistenceFailure, message, false});
}

bool is_contained_path(const std::filesystem::path& root,
                       const std::filesystem::path& candidate) {
    const auto relative = candidate.lexically_relative(root);
    if (relative.empty()) {
        return candidate == root;
    }
    const auto first = relative.begin();
    return first != relative.end() &&
           *first != std::filesystem::path("..");
}

bool is_reparse_or_symlink(const std::filesystem::path& path,
                           std::error_code& error) {
    error.clear();
    if (!std::filesystem::exists(path, error)) {
        return false;
    }
    if (error) {
        return true;
    }
#if defined(_WIN32)
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = std::error_code(
            static_cast<int>(GetLastError()), std::system_category());
        return true;
    }
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return std::filesystem::is_symlink(
        std::filesystem::symlink_status(path, error));
#endif
}

using SessionEvents = std::vector<SessionEvent>;

Result<SessionEvents> decode_session_stream(
    const std::string& bytes,
    const std::string& expected_session_id);

Result<void> validate_session_append(const SessionEvents& events,
                                     const SessionEvent& event) {
    std::optional<SessionState> current;
    if (!events.empty()) {
        auto replayed = replay_session_events(events);
        if (!replayed.has_value()) {
            return Result<void>::failure(replayed.error());
        }
        current = std::move(replayed.value());
    }
    const auto reduced = reduce_session_event(current, event);
    if (!reduced.has_value()) {
        return Result<void>::failure(reduced.error());
    }
    return Result<void>::success();
}

#if defined(_WIN32)

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
            if (valid()) {
                CloseHandle(handle_);
            }
            handle_ = other.handle_;
            other.handle_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    ~UniqueHandle() {
        if (valid()) {
            CloseHandle(handle_);
        }
    }

    HANDLE get() const noexcept { return handle_; }
    bool valid() const noexcept {
        return handle_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_;
};

UniqueHandle open_directory_handle(const std::filesystem::path& path) {
    return UniqueHandle(CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
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
           paths_equal_case_insensitive(child_path,
                                        parent_path / expected_name);
}

bool missing_path_error(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

bool busy_file_error(DWORD error) {
    return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION;
}

Result<SessionEvents> access_session_log(
    const std::filesystem::path& runtime_root,
    const std::string& session_id,
    const SessionEvent* append,
    const std::string* serialized_line) {
    const auto directory =
        runtime_root / "sessions" / std::filesystem::u8path(session_id);
    std::vector<UniqueHandle> directories;
    auto path = directory.root_path();
    auto volume = open_directory_handle(path);
    if (!volume.valid() || !is_plain_directory(volume.get())) {
        return persistence_error<SessionEvents>(
            "session runtime root is unsafe");
    }
    directories.push_back(std::move(volume));
    for (const auto& component : directory.relative_path()) {
        path /= component;
        auto opened = open_directory_handle(path);
        if (!opened.valid()) {
            const auto error = GetLastError();
            if (!missing_path_error(error)) {
                return persistence_error<SessionEvents>(
                    "failed to open session directory");
            }
            if (append == nullptr) {
                return persistence_error<SessionEvents>(
                    "failed to open session log");
            }
            const auto valid = validate_session_append({}, *append);
            if (!valid.has_value()) {
                return Result<SessionEvents>::failure(valid.error());
            }
            if (!ensure_directory_exists(path)) {
                return persistence_error<SessionEvents>(
                    "failed to create session directory");
            }
            auto created = open_directory_handle(path);
            if (!created.valid() || !is_plain_directory(created.get()) ||
                !is_direct_handle_child(directories.back().get(),
                                        created.get(), component)) {
                return persistence_error<SessionEvents>(
                    "session directory is unsafe");
            }
            directories.push_back(std::move(created));
        } else {
            if (!is_plain_directory(opened.get()) ||
                !is_direct_handle_child(directories.back().get(),
                                        opened.get(), component)) {
                return persistence_error<SessionEvents>(
                    "session directory is unsafe");
            }
            directories.push_back(std::move(opened));
        }
    }

    path /= "events.jsonl";
    const DWORD access = GENERIC_READ | FILE_READ_ATTRIBUTES |
                         (append != nullptr ? FILE_APPEND_DATA : 0);
    const auto open_leaf = [&](DWORD disposition) {
        return UniqueHandle(CreateFileW(
            path.c_str(), access, FILE_SHARE_READ, nullptr, disposition,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                FILE_FLAG_WRITE_THROUGH,
            nullptr));
    };
    auto leaf = open_leaf(OPEN_EXISTING);
    bool created = false;
    // A reader holds the log with FILE_SHARE_READ (denying writers) for
    // the duration of its read. Background transcript reads release the
    // file within microseconds; a foreground append should ride that
    // out instead of failing spuriously. Reads keep failing fast — only
    // the append path retries briefly.
    if (!leaf.valid() && append != nullptr && busy_file_error(GetLastError())) {
        for (int attempt = 0; attempt < 20; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            leaf = open_leaf(OPEN_EXISTING);
            if (leaf.valid() || !busy_file_error(GetLastError())) break;
        }
    }
    if (!leaf.valid()) {
        auto error = GetLastError();
        if (busy_file_error(error)) {
            return persistence_error<SessionEvents>("session log is busy");
        }
        if (!missing_path_error(error)) {
            return persistence_error<SessionEvents>(
                "failed to open session log");
        }
        if (append == nullptr) {
            return persistence_error<SessionEvents>(
                "failed to open session log");
        }
        const auto valid = validate_session_append({}, *append);
        if (!valid.has_value()) {
            return Result<SessionEvents>::failure(valid.error());
        }
        leaf = open_leaf(CREATE_NEW);
        if (!leaf.valid() &&
            (GetLastError() == ERROR_FILE_EXISTS ||
             GetLastError() == ERROR_ALREADY_EXISTS)) {
            leaf = open_leaf(OPEN_EXISTING);
        } else if (leaf.valid()) {
            created = true;
        }
        if (!leaf.valid()) {
            error = GetLastError();
            if (busy_file_error(error)) {
                return persistence_error<SessionEvents>(
                    "session log is busy");
            }
            return persistence_error<SessionEvents>(
                "failed to create session log");
        }
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
        !is_direct_handle_child(directories.back().get(), leaf.get(),
                                "events.jsonl")) {
        return persistence_error<SessionEvents>(
            "session log leaf is not a regular file");
    }

    std::string bytes;
    std::array<char, 16 * 1024> buffer{};
    for (;;) {
        DWORD read = 0;
        if (ReadFile(leaf.get(), buffer.data(),
                     static_cast<DWORD>(buffer.size()), &read, nullptr) == 0) {
            return persistence_error<SessionEvents>(
                "failed while reading session log");
        }
        if (read == 0) {
            break;
        }
        bytes.append(buffer.data(), static_cast<std::size_t>(read));
    }
    auto events = created
                      ? Result<SessionEvents>::success({})
                      : decode_session_stream(bytes, session_id);
    if (!events.has_value() || append == nullptr) {
        return events;
    }
    const auto valid = validate_session_append(events.value(), *append);
    if (!valid.has_value()) {
        return Result<SessionEvents>::failure(valid.error());
    }
    if (serialized_line == nullptr) {
        return persistence_error<SessionEvents>(
            "failed to serialize session event");
    }
    std::size_t offset = 0;
    while (offset < serialized_line->size()) {
        const auto remaining = serialized_line->size() - offset;
        const auto chunk = static_cast<DWORD>(
            remaining > std::numeric_limits<DWORD>::max()
                ? std::numeric_limits<DWORD>::max()
                : remaining);
        DWORD written = 0;
        if (WriteFile(leaf.get(), serialized_line->data() + offset, chunk,
                      &written, nullptr) == 0 ||
            written == 0) {
            return persistence_error<SessionEvents>(
                "failed to append complete session record");
        }
        offset += static_cast<std::size_t>(written);
    }
    if (FlushFileBuffers(leaf.get()) == 0) {
        return persistence_error<SessionEvents>(
            "failed to flush session log");
    }
    return events;
}

Result<std::vector<std::string>> secure_list_session_ids(
    const std::filesystem::path& runtime_root) {
    const auto directory = runtime_root / "sessions";
    std::vector<UniqueHandle> directories;
    auto path = directory.root_path();
    auto volume = open_directory_handle(path);
    if (!volume.valid() || !is_plain_directory(volume.get())) {
        return persistence_error<std::vector<std::string>>(
            "session runtime root is unsafe");
    }
    directories.push_back(std::move(volume));
    for (const auto& component : directory.relative_path()) {
        path /= component;
        auto opened = open_directory_handle(path);
        if (!opened.valid()) {
            if (missing_path_error(GetLastError())) {
                return Result<std::vector<std::string>>::success({});
            }
            return persistence_error<std::vector<std::string>>(
                "failed to open session directory");
        }
        if (!is_plain_directory(opened.get()) ||
            !is_direct_handle_child(directories.back().get(), opened.get(),
                                    component)) {
            return persistence_error<std::vector<std::string>>(
                "session directory is unsafe");
        }
        directories.push_back(std::move(opened));
    }

    std::vector<std::string> session_ids;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(directory, error), end;
         !error && iterator != end; iterator.increment(error)) {
        const auto session_id = iterator->path().filename().generic_u8string();
        if (!is_valid_session_id(session_id)) {
            continue;
        }
        if (is_reparse_or_symlink(iterator->path(), error) || error) {
            return persistence_error<std::vector<std::string>>(
                "session directory is unsafe");
        }
        if (iterator->is_directory(error) && !error) {
            session_ids.push_back(session_id);
        }
        if (error) {
            break;
        }
    }
    if (error) {
        return persistence_error<std::vector<std::string>>(
            "failed while listing sessions");
    }
    return Result<std::vector<std::string>>::success(std::move(session_ids));
}

#else

class UniqueFileDescriptor {
public:
    explicit UniqueFileDescriptor(int descriptor = -1)
        : descriptor_(descriptor) {}
    UniqueFileDescriptor(const UniqueFileDescriptor&) = delete;
    UniqueFileDescriptor& operator=(const UniqueFileDescriptor&) = delete;
    UniqueFileDescriptor(UniqueFileDescriptor&& other) noexcept
        : descriptor_(other.descriptor_) {
        other.descriptor_ = -1;
    }
    UniqueFileDescriptor& operator=(UniqueFileDescriptor&& other) noexcept {
        if (this != &other) {
            if (valid()) {
                close(descriptor_);
            }
            descriptor_ = other.descriptor_;
            other.descriptor_ = -1;
        }
        return *this;
    }
    ~UniqueFileDescriptor() {
        if (descriptor_ >= 0) {
            close(descriptor_);
        }
    }

    int get() const noexcept { return descriptor_; }
    bool valid() const noexcept { return descriptor_ >= 0; }

private:
    int descriptor_;
};

bool is_plain_directory(int descriptor) {
    struct stat information {};
    return fstat(descriptor, &information) == 0 &&
           S_ISDIR(information.st_mode);
}

Result<SessionEvents> access_session_log(
    const std::filesystem::path& runtime_root,
    const std::string& session_id,
    const SessionEvent* append,
    const std::string* serialized_line) {
    const auto directory =
        runtime_root / "sessions" / std::filesystem::u8path(session_id);
    std::vector<UniqueFileDescriptor> directories;
    UniqueFileDescriptor volume(
        open(directory.root_path().c_str(),
             O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!volume.valid() || !is_plain_directory(volume.get())) {
        return persistence_error<SessionEvents>(
            "session runtime root is unsafe");
    }
    directories.push_back(std::move(volume));
    for (const auto& component : directory.relative_path()) {
        const int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
        UniqueFileDescriptor opened(
            openat(directories.back().get(), component.c_str(), flags));
        if (!opened.valid()) {
            if (errno != ENOENT) {
                return persistence_error<SessionEvents>(
                    "failed to open session directory");
            }
            if (append == nullptr) {
                return persistence_error<SessionEvents>(
                    "failed to open session log");
            }
            const auto valid = validate_session_append({}, *append);
            if (!valid.has_value()) {
                return Result<SessionEvents>::failure(valid.error());
            }
            if (mkdirat(directories.back().get(), component.c_str(), 0700) !=
                    0 &&
                errno != EEXIST) {
                return persistence_error<SessionEvents>(
                    "failed to create session directory");
            }
            UniqueFileDescriptor created(
                openat(directories.back().get(), component.c_str(), flags));
            if (!created.valid() || !is_plain_directory(created.get())) {
                return persistence_error<SessionEvents>(
                    "session directory is unsafe");
            }
            directories.push_back(std::move(created));
        } else {
            if (!is_plain_directory(opened.get())) {
                return persistence_error<SessionEvents>(
                    "session directory is unsafe");
            }
            directories.push_back(std::move(opened));
        }
    }

    const int flags = (append != nullptr ? O_RDWR | O_APPEND : O_RDONLY) |
                      O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    std::vector<UniqueFileDescriptor> leaves;
    UniqueFileDescriptor existing(
        openat(directories.back().get(), "events.jsonl", flags));
    bool created = false;
    if (!existing.valid()) {
        if (errno != ENOENT) {
            return persistence_error<SessionEvents>(
                "failed to open session log");
        }
        if (append == nullptr) {
            return persistence_error<SessionEvents>(
                "failed to open session log");
        }
        const auto valid = validate_session_append({}, *append);
        if (!valid.has_value()) {
            return Result<SessionEvents>::failure(valid.error());
        }
        UniqueFileDescriptor leaf(openat(
            directories.back().get(), "events.jsonl",
            flags | O_CREAT | O_EXCL, 0600));
        if (!leaf.valid() && errno == EEXIST) {
            leaf = UniqueFileDescriptor(
                openat(directories.back().get(), "events.jsonl", flags));
        } else if (leaf.valid()) {
            created = true;
        }
        if (!leaf.valid()) {
            return persistence_error<SessionEvents>(
                "failed to create session log");
        }
        leaves.push_back(std::move(leaf));
    } else {
        leaves.push_back(std::move(existing));
    }
    const auto leaf = leaves.back().get();
    struct stat information {};
    if (fstat(leaf, &information) != 0 ||
        !S_ISREG(information.st_mode) || information.st_nlink != 1) {
        return persistence_error<SessionEvents>(
            "session log leaf is not a regular file");
    }
    if (flock(leaf, (append != nullptr ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0) {
        return persistence_error<SessionEvents>("session log is busy");
    }
    std::string bytes;
    std::array<char, 16 * 1024> buffer{};
    for (;;) {
        const auto read_count = read(leaf, buffer.data(), buffer.size());
        if (read_count < 0 && errno == EINTR) {
            continue;
        }
        if (read_count < 0) {
            return persistence_error<SessionEvents>(
                "failed while reading session log");
        }
        if (read_count == 0) {
            break;
        }
        bytes.append(buffer.data(), static_cast<std::size_t>(read_count));
    }
    auto events = created
                      ? Result<SessionEvents>::success({})
                      : decode_session_stream(bytes, session_id);
    if (!events.has_value() || append == nullptr) {
        return events;
    }
    const auto valid = validate_session_append(events.value(), *append);
    if (!valid.has_value()) {
        return Result<SessionEvents>::failure(valid.error());
    }
    if (serialized_line == nullptr) {
        return persistence_error<SessionEvents>(
            "failed to serialize session event");
    }
    std::size_t offset = 0;
    while (offset < serialized_line->size()) {
        const auto written =
            write(leaf, serialized_line->data() + offset,
                  serialized_line->size() - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return persistence_error<SessionEvents>(
                "failed to append complete session record");
        }
        offset += static_cast<std::size_t>(written);
    }
    if (fsync(leaf) != 0) {
        return persistence_error<SessionEvents>(
            "failed to flush session log");
    }
    return events;
}

class UniqueDirectoryStream {
public:
    explicit UniqueDirectoryStream(DIR* stream = nullptr) : stream_(stream) {}
    UniqueDirectoryStream(const UniqueDirectoryStream&) = delete;
    UniqueDirectoryStream& operator=(const UniqueDirectoryStream&) = delete;
    ~UniqueDirectoryStream() {
        if (stream_ != nullptr) {
            closedir(stream_);
        }
    }
    DIR* get() const noexcept { return stream_; }
    bool valid() const noexcept { return stream_ != nullptr; }

private:
    DIR* stream_;
};

Result<std::vector<std::string>> secure_list_session_ids(
    const std::filesystem::path& runtime_root) {
    const auto directory = runtime_root / "sessions";
    std::vector<UniqueFileDescriptor> directories;
    UniqueFileDescriptor volume(
        open(directory.root_path().c_str(),
             O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!volume.valid() || !is_plain_directory(volume.get())) {
        return persistence_error<std::vector<std::string>>(
            "session runtime root is unsafe");
    }
    directories.push_back(std::move(volume));
    for (const auto& component : directory.relative_path()) {
        const int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
        UniqueFileDescriptor opened(
            openat(directories.back().get(), component.c_str(), flags));
        if (!opened.valid()) {
            if (errno == ENOENT) {
                return Result<std::vector<std::string>>::success({});
            }
            return persistence_error<std::vector<std::string>>(
                "failed to open session directory");
        }
        if (!is_plain_directory(opened.get())) {
            return persistence_error<std::vector<std::string>>(
                "session directory is unsafe");
        }
        directories.push_back(std::move(opened));
    }

    const auto duplicate = dup(directories.back().get());
    if (duplicate < 0) {
        return persistence_error<std::vector<std::string>>(
            "failed while listing sessions");
    }
    UniqueDirectoryStream stream(fdopendir(duplicate));
    if (!stream.valid()) {
        close(duplicate);
        return persistence_error<std::vector<std::string>>(
            "failed while listing sessions");
    }
    std::vector<std::string> session_ids;
    errno = 0;
    while (const auto* entry = readdir(stream.get())) {
        const std::string session_id(entry->d_name);
        if (!is_valid_session_id(session_id)) {
            continue;
        }
        struct stat information {};
        if (fstatat(directories.back().get(), entry->d_name, &information,
                    AT_SYMLINK_NOFOLLOW) != 0) {
            return persistence_error<std::vector<std::string>>(
                "failed while listing sessions");
        }
        if (S_ISLNK(information.st_mode)) {
            return persistence_error<std::vector<std::string>>(
                "session directory is unsafe");
        }
        if (S_ISDIR(information.st_mode)) {
            session_ids.push_back(session_id);
        }
    }
    if (errno != 0) {
        return persistence_error<std::vector<std::string>>(
            "failed while listing sessions");
    }
    return Result<std::vector<std::string>>::success(std::move(session_ids));
}

#endif

Result<std::vector<SessionEvent>> decode_session_stream(
    const std::string& bytes,
    const std::string& expected_session_id) {
    if (bytes.empty() || bytes.back() != '\n') {
        return persistence_error<std::vector<SessionEvent>>(
            "session event log is empty or truncated");
    }
    std::istringstream input(bytes);
    std::vector<SessionEvent> events;
    std::string line;
    try {
        while (std::getline(input, line)) {
            if (line.empty()) {
                return persistence_error<std::vector<SessionEvent>>(
                    "session event log contains an empty record");
            }
            bool duplicate_key = false;
            std::vector<std::unordered_set<std::string>> object_keys;
            const auto json = nlohmann::json::parse(
                line,
                [&](int, nlohmann::json::parse_event_t event,
                    nlohmann::json& parsed) {
                    if (event == nlohmann::json::parse_event_t::object_start) {
                        object_keys.emplace_back();
                    } else if (event ==
                               nlohmann::json::parse_event_t::key) {
                        if (object_keys.empty() ||
                            !object_keys.back()
                                 .insert(parsed.get<std::string>())
                                 .second) {
                            duplicate_key = true;
                        }
                    } else if (event ==
                               nlohmann::json::parse_event_t::object_end) {
                        if (object_keys.empty()) {
                            duplicate_key = true;
                        } else {
                            object_keys.pop_back();
                        }
                    }
                    return true;
                });
            if (duplicate_key || !object_keys.empty()) {
                return persistence_error<std::vector<SessionEvent>>(
                    "session event record contains duplicate keys");
            }
            auto decoded = session_event_from_json(json);
            if (!decoded.has_value() ||
                decoded.value().session_id != expected_session_id) {
                return persistence_error<std::vector<SessionEvent>>(
                    "session event record is invalid");
            }
            events.push_back(std::move(decoded.value()));
        }
    } catch (const std::exception&) {
        return persistence_error<std::vector<SessionEvent>>(
            "session event log could not be parsed");
    }
    if (events.empty() || !replay_session_events(events).has_value()) {
        return persistence_error<std::vector<SessionEvent>>(
            "session event trace is invalid");
    }
    return Result<std::vector<SessionEvent>>::success(std::move(events));
}

}  // namespace

JsonlSessionStore::JsonlSessionStore(std::filesystem::path runtime_root)
    : runtime_root_(std::move(runtime_root)) {}

Result<std::filesystem::path> JsonlSessionStore::event_path(
    const std::string& session_id) const {
    if (!is_valid_session_id(session_id)) {
        return persistence_error<std::filesystem::path>(
            "invalid session ID for event storage");
    }
    std::error_code error;
    const auto sessions_root = std::filesystem::absolute(
        runtime_root_ / "sessions", error).lexically_normal();
    if (error) {
        return persistence_error<std::filesystem::path>(
            "failed to resolve session runtime root");
    }
    const auto candidate =
        (sessions_root / std::filesystem::u8path(session_id) /
         "events.jsonl")
            .lexically_normal();
    if (!is_contained_path(sessions_root, candidate)) {
        return persistence_error<std::filesystem::path>(
            "session event path escapes runtime root");
    }
    return Result<std::filesystem::path>::success(candidate);
}

Result<void> JsonlSessionStore::append(const SessionEvent& event) {
    try {
        const auto resolved = event_path(event.session_id);
        if (!resolved.has_value()) {
            return Result<void>::failure(resolved.error());
        }
        std::error_code error;
        const auto absolute_root = std::filesystem::absolute(
            runtime_root_, error).lexically_normal();
        if (error) {
            return persistence_void_error(
                "failed to resolve session runtime root");
        }
        auto serialized = session_event_to_json(event).dump();
        serialized.push_back('\n');
        auto accessed = access_session_log(
            absolute_root, event.session_id, &event, &serialized);
        if (!accessed.has_value()) {
            return Result<void>::failure(accessed.error());
        }
        return Result<void>::success();
    } catch (const std::exception&) {
        return persistence_void_error(
            "failed to serialize or append session event");
    }
}

Result<std::vector<SessionEvent>> JsonlSessionStore::read_session(
    const std::string& session_id) const {
    const auto resolved = event_path(session_id);
    if (!resolved.has_value()) {
        return Result<std::vector<SessionEvent>>::failure(resolved.error());
    }
    std::error_code error;
    const auto absolute_root = std::filesystem::absolute(
        runtime_root_, error).lexically_normal();
    if (error) {
        return persistence_error<std::vector<SessionEvent>>(
            "failed to resolve session runtime root");
    }
    return access_session_log(absolute_root, session_id, nullptr, nullptr);
}

Result<std::vector<SessionState>> JsonlSessionStore::list_sessions() const {
    std::error_code error;
    const auto absolute_root = std::filesystem::absolute(
        runtime_root_, error).lexically_normal();
    if (error) {
        return persistence_error<std::vector<SessionState>>(
            "failed to resolve sessions root");
    }
    auto session_ids = secure_list_session_ids(absolute_root);
    if (!session_ids.has_value()) {
        return Result<std::vector<SessionState>>::failure(
            session_ids.error());
    }

    std::vector<SessionState> states;
    for (const auto& session_id : session_ids.value()) {
        auto loaded = read_session(session_id);
        if (!loaded.has_value()) {
            return Result<std::vector<SessionState>>::failure(loaded.error());
        }
        auto replayed = replay_session_events(loaded.value());
        if (!replayed.has_value()) {
            return Result<std::vector<SessionState>>::failure(replayed.error());
        }
        states.push_back(std::move(replayed.value()));
    }
    std::sort(states.begin(), states.end(),
              [](const SessionState& left, const SessionState& right) {
                  if (left.last_timestamp_utc != right.last_timestamp_utc) {
                      return left.last_timestamp_utc > right.last_timestamp_utc;
                  }
                  return left.session_id < right.session_id;
              });
    return Result<std::vector<SessionState>>::success(std::move(states));
}

}  // namespace agent

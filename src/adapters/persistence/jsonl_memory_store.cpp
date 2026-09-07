#include "adapters/persistence/jsonl_memory_store.h"
#include "adapters/persistence/memory_event_json.h"
#include "application/memory_reducer.h"

#include <array>
#include <limits>
#include <nlohmann/json.hpp>
#include <sstream>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace agent {
namespace {
template <typename T> Result<T> failure(const char *message) {
    return Result<T>::failure({ErrorCode::PersistenceFailure, message, false});
}
using Events = std::vector<MemoryEvent>;

Result<Events> decode(const std::string &bytes) {
    if (bytes.empty() || bytes.back() != '\n')
        return failure<Events>("memory log is empty or truncated");
    Events events;
    std::istringstream input(bytes);
    std::string line;
    try {
        while (std::getline(input, line)) {
            if (line.empty())
                return failure<Events>("memory log contains an empty record");
            bool duplicate = false;
            std::vector<std::unordered_set<std::string>> keys;
            const auto json = nlohmann::json::parse(
                line, [&keys, &duplicate](int, nlohmann::json::parse_event_t event,
                                          nlohmann::json &parsed) {
                    using Parse = nlohmann::json::parse_event_t;
                    if (event == Parse::object_start)
                        keys.emplace_back();
                    else if (event == Parse::key) {
                        if (keys.empty() ||
                            !keys.back().insert(parsed.get<std::string>()).second)
                            duplicate = true;
                    } else if (event == Parse::object_end) {
                        if (keys.empty())
                            duplicate = true;
                        else
                            keys.pop_back();
                    }
                    return true;
                });
            if (duplicate || !keys.empty())
                return failure<Events>("memory record has duplicate keys");
            auto event = memory_event_from_json(json);
            if (!event.has_value())
                return failure<Events>("memory record is invalid");
            events.push_back(std::move(event.value()));
        }
    } catch (const std::exception &) {
        return failure<Events>("memory log could not be parsed");
    }
    if (!replay_memory_events(events).has_value())
        return failure<Events>("memory event trace is invalid");
    return Result<Events>::success(std::move(events));
}

Result<void> validate_append(const Events &events, const MemoryEvent &event) {
    auto current = replay_memory_events(events);
    if (!current.has_value())
        return Result<void>::failure(current.error());
    const auto next = reduce_memory_event(current.value(), event);
    if (!next.has_value())
        return Result<void>::failure(next.error());
    return Result<void>::success();
}

#if defined(_WIN32)
// These native checks mirror JsonlSessionStore: pin plain directories without
// delete sharing, verify direct handle ancestry, and disallow reparse/hard-link
// leaves.
class Handle {
  public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    Handle(Handle &&other) noexcept : value_(other.value_) {
        other.value_ = INVALID_HANDLE_VALUE;
    }
    ~Handle() {
        if (valid())
            CloseHandle(value_);
    }
    HANDLE get() const { return value_; }
    bool valid() const { return value_ != INVALID_HANDLE_VALUE; }

  private:
    HANDLE value_;
};

Handle open_directory(const std::filesystem::path &path) {
    return Handle(CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
}
bool plain_directory(HANDLE handle) {
    FILE_ATTRIBUTE_TAG_INFO info{};
    return GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info,
                                        sizeof(info)) != 0 &&
           (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}
bool final_path(HANDLE handle, std::filesystem::path &output) {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const auto count = GetFinalPathNameByHandleW(
            handle, buffer.data(), static_cast<DWORD>(buffer.size()),
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (count == 0)
            return false;
        if (count < buffer.size()) {
            output = std::filesystem::path(std::wstring(buffer.data(), count));
            return true;
        }
        buffer.resize(static_cast<std::size_t>(count) + 1);
    }
}
bool same_path(const std::filesystem::path &a, const std::filesystem::path &b) {
    const auto left = a.lexically_normal().native(),
               right = b.lexically_normal().native();
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) ==
           CSTR_EQUAL;
}
bool direct_child(HANDLE parent, HANDLE child, const std::filesystem::path &name) {
    std::filesystem::path parent_path, child_path;
    return final_path(parent, parent_path) && final_path(child, child_path) &&
           same_path(child_path, parent_path / name);
}
bool missing_error(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

Result<Events> access_log(const std::filesystem::path &root,
                          const MemoryEvent *append) {
    const auto directory = root / "memories";
    std::vector<Handle> directories;
    auto path = directory.root_path();
    auto volume = open_directory(path);
    if (!volume.valid() || !plain_directory(volume.get()))
        return failure<Events>("memory runtime root is unsafe");
    directories.push_back(std::move(volume));
    for (const auto &component : directory.relative_path()) {
        path /= component;
        auto opened = open_directory(path);
        if (!opened.valid()) {
            const auto error = GetLastError();
            if (!missing_error(error))
                return failure<Events>("failed to open memory directory");
            if (append == nullptr)
                return Result<Events>::success({});
            const auto valid = validate_append({}, *append);
            if (!valid.has_value())
                return Result<Events>::failure(valid.error());
            if (CreateDirectoryW(path.c_str(), nullptr) == 0 &&
                GetLastError() != ERROR_ALREADY_EXISTS)
                return failure<Events>("failed to create memory directory");
            auto created = open_directory(path);
            if (!created.valid() || !plain_directory(created.get()) ||
                !direct_child(directories.back().get(), created.get(), component))
                return failure<Events>("memory directory is unsafe");
            directories.push_back(std::move(created));
        } else {
            if (!plain_directory(opened.get()) ||
                !direct_child(directories.back().get(), opened.get(), component))
                return failure<Events>("memory directory is unsafe");
            directories.push_back(std::move(opened));
        }
    }
    path /= "events.jsonl";
    const DWORD access = GENERIC_READ | FILE_READ_ATTRIBUTES |
                         (append != nullptr ? FILE_APPEND_DATA : 0);
    const auto open_leaf = [&](DWORD disposition) {
        return Handle(CreateFileW(path.c_str(), access, FILE_SHARE_READ, nullptr,
                                  disposition,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                                      FILE_FLAG_WRITE_THROUGH,
                                  nullptr));
    };
    std::vector<Handle> leaves;
    auto existing = open_leaf(OPEN_EXISTING);
    bool created = false;
    if (!existing.valid()) {
        if (!missing_error(GetLastError()))
            return failure<Events>("failed to open memory log");
        if (append == nullptr)
            return Result<Events>::success({});
        const auto valid = validate_append({}, *append);
        if (!valid.has_value())
            return Result<Events>::failure(valid.error());
        auto leaf = open_leaf(CREATE_NEW);
        if (!leaf.valid())
            return failure<Events>("failed to create memory log");
        leaves.push_back(std::move(leaf));
        created = true;
    } else
        leaves.push_back(std::move(existing));
    const auto leaf = leaves.back().get();
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandleEx(leaf, FileAttributeTagInfo, &attributes,
                                     sizeof(attributes)) == 0 ||
        GetFileInformationByHandle(leaf, &info) == 0 ||
        GetFileType(leaf) != FILE_TYPE_DISK ||
        (attributes.FileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        info.nNumberOfLinks != 1 ||
        !direct_child(directories.back().get(), leaf, "events.jsonl"))
        return failure<Events>("memory log leaf is unsafe");
    std::string bytes;
    std::array<char, 16 * 1024> buffer{};
    for (;;) {
        DWORD count = 0;
        if (ReadFile(leaf, buffer.data(), static_cast<DWORD>(buffer.size()), &count,
                     nullptr) == 0)
            return failure<Events>("failed to read memory log");
        if (count == 0)
            break;
        bytes.append(buffer.data(), count);
    }
    auto events = created ? Result<Events>::success({}) : decode(bytes);
    if (!events.has_value() || append == nullptr)
        return events;
    const auto valid = validate_append(events.value(), *append);
    if (!valid.has_value())
        return Result<Events>::failure(valid.error());
    auto line = memory_event_to_json(*append).dump() + '\n';
    for (std::size_t offset = 0; offset < line.size();) {
        const auto chunk = static_cast<DWORD>(
            (std::min)(line.size() - offset,
                       static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD count = 0;
        if (WriteFile(leaf, line.data() + offset, chunk, &count, nullptr) == 0 ||
            count == 0)
            return failure<Events>("failed to append complete memory record");
        offset += count;
    }
    if (FlushFileBuffers(leaf) == 0)
        return failure<Events>("failed to flush memory log");
    return events;
}
#else
class Descriptor {
  public:
    explicit Descriptor(int value = -1) : value_(value) {}
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
    Descriptor(Descriptor &&other) noexcept : value_(other.value_) {
        other.value_ = -1;
    }
    ~Descriptor() {
        if (valid())
            close(value_);
    }
    int get() const { return value_; }
    bool valid() const { return value_ >= 0; }

  private:
    int value_;
};
bool plain_directory(int handle) {
    struct stat info {};
    return fstat(handle, &info) == 0 && S_ISDIR(info.st_mode);
}
Result<Events> access_log(const std::filesystem::path &root,
                          const MemoryEvent *append) {
    const auto directory = root / "memories";
    std::vector<Descriptor> directories;
    Descriptor volume(open(directory.root_path().c_str(),
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!volume.valid() || !plain_directory(volume.get()))
        return failure<Events>("memory runtime root is unsafe");
    directories.push_back(std::move(volume));
    for (const auto &component : directory.relative_path()) {
        const int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
        Descriptor opened(openat(directories.back().get(), component.c_str(), flags));
        if (!opened.valid()) {
            if (errno != ENOENT)
                return failure<Events>("failed to open memory directory");
            if (append == nullptr)
                return Result<Events>::success({});
            const auto valid = validate_append({}, *append);
            if (!valid.has_value())
                return Result<Events>::failure(valid.error());
            if (mkdirat(directories.back().get(), component.c_str(), 0700) != 0 &&
                errno != EEXIST)
                return failure<Events>("failed to create memory directory");
            Descriptor created(
                openat(directories.back().get(), component.c_str(), flags));
            if (!created.valid() || !plain_directory(created.get()))
                return failure<Events>("memory directory is unsafe");
            directories.push_back(std::move(created));
        } else {
            if (!plain_directory(opened.get()))
                return failure<Events>("memory directory is unsafe");
            directories.push_back(std::move(opened));
        }
    }
    const int flags =
        (append ? O_RDWR | O_APPEND : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    Descriptor existing(openat(directories.back().get(), "events.jsonl", flags));
    std::vector<Descriptor> leaves;
    bool created = false;
    if (!existing.valid()) {
        if (errno != ENOENT)
            return failure<Events>("failed to open memory log");
        if (append == nullptr)
            return Result<Events>::success({});
        const auto valid = validate_append({}, *append);
        if (!valid.has_value())
            return Result<Events>::failure(valid.error());
        Descriptor leaf(openat(directories.back().get(), "events.jsonl",
                               flags | O_CREAT | O_EXCL, 0600));
        if (!leaf.valid())
            return failure<Events>("failed to create memory log");
        leaves.push_back(std::move(leaf));
        created = true;
    } else
        leaves.push_back(std::move(existing));
    const auto leaf = leaves.back().get();
    struct stat info {};
    if (fstat(leaf, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1)
        return failure<Events>("memory log leaf is unsafe");
    // Cooperating readers/writers hold the lock through validation and append.
    if (flock(leaf, (append ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0)
        return failure<Events>("memory log is busy");
    std::string bytes;
    std::array<char, 16 * 1024> buffer{};
    for (;;) {
        const auto count = read(leaf, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            return failure<Events>("failed to read memory log");
        if (count == 0)
            break;
        bytes.append(buffer.data(), static_cast<std::size_t>(count));
    }
    auto events = created ? Result<Events>::success({}) : decode(bytes);
    if (!events.has_value() || append == nullptr)
        return events;
    const auto valid = validate_append(events.value(), *append);
    if (!valid.has_value())
        return Result<Events>::failure(valid.error());
    auto line = memory_event_to_json(*append).dump() + '\n';
    for (std::size_t offset = 0; offset < line.size();) {
        const auto count = write(leaf, line.data() + offset, line.size() - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return failure<Events>("failed to append complete memory record");
        offset += static_cast<std::size_t>(count);
    }
    if (fsync(leaf) != 0)
        return failure<Events>("failed to flush memory log");
    return events;
}
#endif
} // namespace

JsonlMemoryStore::JsonlMemoryStore(std::filesystem::path runtime_root)
    : runtime_root_(std::move(runtime_root)) {}

Result<std::filesystem::path> JsonlMemoryStore::event_path() const {
    std::error_code error;
    const auto root =
        std::filesystem::absolute(runtime_root_, error).lexically_normal();
    if (error || runtime_root_.empty())
        return failure<std::filesystem::path>("failed to resolve memory runtime root");
    return Result<std::filesystem::path>::success(root / "memories" / "events.jsonl");
}

Result<void> JsonlMemoryStore::append(const MemoryEvent &event) {
    try {
        const auto valid = validate_memory_event(event);
        if (!valid.has_value())
            return valid;
        const auto path = event_path();
        if (!path.has_value())
            return Result<void>::failure(path.error());
        auto result = access_log(path.value().parent_path().parent_path(), &event);
        if (!result.has_value())
            return Result<void>::failure(result.error());
        return Result<void>::success();
    } catch (const std::exception &) {
        return failure<void>("failed to serialize or append memory event");
    }
}

Result<std::vector<MemoryEvent>> JsonlMemoryStore::read_all() const {
    try {
        const auto path = event_path();
        if (!path.has_value())
            return Result<Events>::failure(path.error());
        return access_log(path.value().parent_path().parent_path(), nullptr);
    } catch (const std::exception &) {
        return failure<Events>("failed to read memory log");
    }
}

Result<MemoryState> JsonlMemoryStore::read_state() const {
    const auto events = read_all();
    if (!events.has_value())
        return Result<MemoryState>::failure(events.error());
    return replay_memory_events(events.value());
}
} // namespace agent

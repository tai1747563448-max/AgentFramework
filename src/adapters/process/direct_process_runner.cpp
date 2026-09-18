#include "adapters/process/direct_process_runner.h"

#include "adapters/workspace/workspace_text.h"
#include "ports/sandbox.h"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace agent {
namespace {

RuntimeError invalid_request() {
    return {ErrorCode::InvalidInput, "invalid process request", false};
}

RuntimeError start_failure() {
    return {ErrorCode::DependencyUnavailable,
            "process could not be started", false};
}

RuntimeError execution_failure() {
    return {ErrorCode::DependencyUnavailable,
            "process execution failed", false};
}

bool sensitive_environment_name(std::string_view name) {
    std::string upper(name);
    std::transform(upper.begin(), upper.end(), upper.begin(), [](char value) {
        return value >= 'a' && value <= 'z'
                   ? static_cast<char>(value - ('a' - 'A'))
                   : value;
    });
    for (const std::string_view marker :
         {"KEY", "TOKEN", "SECRET", "PASSWORD", "PASSWD", "CREDENTIAL",
          "AUTH"}) {
        if (upper.find(marker) != std::string::npos) {
            return true;
        }
    }
    return false;
}

class BoundedCollector final {
public:
    explicit BoundedCollector(std::size_t budget)
        : budget_(budget),
          head_limit_(budget == 0 ? 0 : std::max<std::size_t>(1, budget / 4)),
          tail_limit_(budget - head_limit_) {}

    void append(const char* bytes, std::size_t size) {
        for (std::size_t index = 0; index < size; ++index) {
            ++total_;
            if (head_.size() < head_limit_) {
                head_.push_back(bytes[index]);
            } else if (tail_limit_ != 0) {
                tail_.push_back(bytes[index]);
                if (tail_.size() > tail_limit_) {
                    tail_.pop_front();
                }
            }
        }
    }

    std::string bytes() const {
        std::string output;
        output.reserve(head_.size() + tail_.size());
        output += head_;
        output.append(tail_.begin(), tail_.end());
        return output;
    }

    bool truncated() const noexcept {
        return total_ > budget_;
    }

private:
    std::size_t budget_;
    std::size_t head_limit_;
    std::size_t tail_limit_;
    std::size_t total_{0};
    std::string head_;
    std::deque<char> tail_;
};

bool valid_utf8_unit(std::string_view text,
                     std::size_t offset,
                     std::size_t& length) {
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first >= 1U && first <= 0x7FU) {
        length = 1;
        return true;
    }
    const auto continuation = [&](std::size_t index) {
        if (index >= text.size()) {
            return false;
        }
        const auto byte = static_cast<unsigned char>(text[index]);
        return byte >= 0x80U && byte <= 0xBFU;
    };
    if (first >= 0xC2U && first <= 0xDFU && continuation(offset + 1)) {
        length = 2;
        return true;
    }
    if (first >= 0xE0U && first <= 0xEFU && offset + 2 < text.size()) {
        const auto second = static_cast<unsigned char>(text[offset + 1]);
        if (continuation(offset + 1) && continuation(offset + 2) &&
            (first != 0xE0U || second >= 0xA0U) &&
            (first != 0xEDU || second <= 0x9FU)) {
            length = 3;
            return true;
        }
    }
    if (first >= 0xF0U && first <= 0xF4U && offset + 3 < text.size()) {
        const auto second = static_cast<unsigned char>(text[offset + 1]);
        if (continuation(offset + 1) && continuation(offset + 2) &&
            continuation(offset + 3) &&
            (first != 0xF0U || second >= 0x90U) &&
            (first != 0xF4U || second <= 0x8FU)) {
            length = 4;
            return true;
        }
    }
    length = 1;
    return false;
}

struct NormalizedOutput final {
    std::string text;
    bool truncated{false};
};

NormalizedOutput normalized_utf8(const std::string& bytes,
                                 std::size_t budget) {
    std::string output;
    output.reserve(std::min(bytes.size(), budget));
    constexpr std::string_view replacement = "\xEF\xBF\xBD";
    for (std::size_t offset = 0; offset < bytes.size();) {
        std::size_t length = 1;
        if (valid_utf8_unit(bytes, offset, length)) {
            if (length > budget - output.size()) {
                return {std::move(output), true};
            }
            output.append(bytes, offset, length);
            offset += length;
        } else {
            if (replacement.size() > budget - output.size()) {
                return {std::move(output), true};
            }
            output.append(replacement);
            ++offset;
        }
    }
    return {std::move(output), false};
}

bool valid_request(const ProcessRequest& request) {
    if (request.program.empty() ||
        !workspace::is_strict_utf8_text(request.program) ||
        request.working_directory.empty() || request.timeout_ms <= 0 ||
        request.timeout_ms > 86'400'000 || request.max_stdout_bytes == 0 ||
        request.max_stderr_bytes == 0 ||
        request.max_stdout_bytes > 4U * 1024U * 1024U ||
        request.max_stderr_bytes > 4U * 1024U * 1024U ||
        (!request.stdin_utf8.empty() &&
         !workspace::is_strict_utf8_text(request.stdin_utf8))) {
        return false;
    }
    for (const auto& argument : request.arguments) {
        if (!argument.empty() && !workspace::is_strict_utf8_text(argument)) {
            return false;
        }
        if (argument.find('\0') != std::string::npos) {
            return false;
        }
    }
    for (const auto& entry : request.environment_overrides) {
        if (entry.first.empty() || entry.first.find('=') != std::string::npos ||
            !workspace::is_strict_utf8_text(entry.first) ||
            (!entry.second.empty() &&
             !workspace::is_strict_utf8_text(entry.second))) {
            return false;
        }
    }
    std::error_code error;
    return std::filesystem::is_directory(request.working_directory, error) &&
           !error;
}

#if defined(_WIN32)

class UniqueHandle final {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : handle_(handle) {}
    ~UniqueHandle() { reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }
    HANDLE get() const noexcept { return handle_; }
    HANDLE release() noexcept {
        const auto value = handle_;
        handle_ = nullptr;
        return value;
    }
    void reset(HANDLE handle = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }
    explicit operator bool() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_{nullptr};
};

std::optional<std::wstring> to_wide(std::string_view utf8) {
    if (utf8.empty()) {
        return std::wstring{};
    }
    if (utf8.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), nullptr, 0);
    if (count <= 0) {
        return std::nullopt;
    }
    std::wstring wide(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                            static_cast<int>(utf8.size()), wide.data(),
                            count) != count) {
        return std::nullopt;
    }
    return wide;
}

std::wstring quote_argument(const std::wstring& value) {
    if (!value.empty() && value.find_first_of(L" \t\n\v\"") ==
                              std::wstring::npos) {
        return value;
    }
    std::wstring quoted(1, L'"');
    std::size_t slashes = 0;
    for (const auto character : value) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        if (character == L'"') {
            quoted.append(slashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            slashes = 0;
            continue;
        }
        quoted.append(slashes, L'\\');
        slashes = 0;
        quoted.push_back(character);
    }
    quoted.append(slashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::optional<std::wstring> environment_value(const wchar_t* name) {
    SetLastError(ERROR_SUCCESS);
    DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) {
        return std::nullopt;
    }
    std::wstring value(static_cast<std::size_t>(size), L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), size);
    if (written >= size) {
        return std::nullopt;
    }
    value.resize(written);
    return value;
}

std::optional<std::vector<wchar_t>> environment_block(
    const ProcessRequest& request) {
    static constexpr const wchar_t* kAllowed[] = {
        L"PATH", L"PATHEXT", L"SYSTEMROOT", L"WINDIR", L"COMSPEC",
        L"TEMP", L"TMP", L"USERPROFILE", L"HOMEDRIVE", L"HOMEPATH",
        L"APPDATA", L"LOCALAPPDATA", L"PROGRAMDATA", L"PROGRAMFILES",
        L"PROGRAMFILES(X86)", L"NUMBER_OF_PROCESSORS",
        L"PROCESSOR_ARCHITECTURE", L"OS", L"SYSTEMDRIVE", L"CC", L"CXX",
        L"INCLUDE", L"LIB", L"LIBPATH", L"CMAKE_GENERATOR",
        L"CMAKE_GENERATOR_PLATFORM", L"CMAKE_GENERATOR_TOOLSET"};
    std::map<std::wstring, std::wstring> values;
    for (const auto* name : kAllowed) {
        auto value = environment_value(name);
        if (value.has_value()) {
            values.emplace(name, std::move(*value));
        }
    }
    for (const auto& override_value : request.environment_overrides) {
        if (sensitive_environment_name(override_value.first)) {
            continue;
        }
        auto name = to_wide(override_value.first);
        auto value = to_wide(override_value.second);
        if (!name.has_value() || !value.has_value()) {
            return std::nullopt;
        }
        std::transform(name->begin(), name->end(), name->begin(),
                       [](wchar_t character) {
                           return static_cast<wchar_t>(towupper(character));
                       });
        values[*name] = std::move(*value);
    }
    std::vector<wchar_t> block;
    for (const auto& entry : values) {
        block.insert(block.end(), entry.first.begin(), entry.first.end());
        block.push_back(L'=');
        block.insert(block.end(), entry.second.begin(), entry.second.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

bool create_pipe(UniqueHandle& read, UniqueHandle& write, bool parent_reads) {
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE raw_read = nullptr;
    HANDLE raw_write = nullptr;
    if (CreatePipe(&raw_read, &raw_write, &security, 0) == FALSE) {
        return false;
    }
    read.reset(raw_read);
    write.reset(raw_write);
    HANDLE parent_end = parent_reads ? read.get() : write.get();
    return SetHandleInformation(parent_end, HANDLE_FLAG_INHERIT, 0) != FALSE;
}

void read_handle(HANDLE handle, BoundedCollector& output) {
    char buffer[4096];
    for (;;) {
        DWORD count = 0;
        if (ReadFile(handle, buffer, sizeof(buffer), &count, nullptr) == FALSE ||
            count == 0) {
            break;
        }
        output.append(buffer, static_cast<std::size_t>(count));
    }
}

void write_handle(HANDLE raw_handle, const std::string& input) {
    UniqueHandle handle(raw_handle);
    std::size_t offset = 0;
    while (offset < input.size()) {
        const auto remaining = std::min<std::size_t>(
            input.size() - offset, std::numeric_limits<DWORD>::max());
        DWORD written = 0;
        if (WriteFile(handle.get(), input.data() + offset,
                      static_cast<DWORD>(remaining), &written, nullptr) == FALSE ||
            written == 0) {
            break;
        }
        offset += static_cast<std::size_t>(written);
    }
}

Result<ProcessOutput> run_native(const ProcessRequest& request,
                          const SandboxedCommand& wrapped) {
    const auto wide_program = to_wide(wrapped.program);
    const auto wide_cwd = to_wide(request.working_directory.generic_u8string());
    auto environment = environment_block(request);
    if (!wide_program.has_value() || !wide_cwd.has_value() ||
        !environment.has_value()) {
        return Result<ProcessOutput>::failure(invalid_request());
    }
    std::wstring command = quote_argument(*wide_program);
    for (const auto& argument : wrapped.arguments) {
        const auto wide_argument = to_wide(argument);
        if (!wide_argument.has_value()) {
            return Result<ProcessOutput>::failure(invalid_request());
        }
        command.push_back(L' ');
        command += quote_argument(*wide_argument);
    }

    UniqueHandle stdout_read, stdout_write;
    UniqueHandle stderr_read, stderr_write;
    UniqueHandle stdin_read, stdin_write;
    if (!create_pipe(stdout_read, stdout_write, true) ||
        !create_pipe(stderr_read, stderr_write, true) ||
        !create_pipe(stdin_read, stdin_write, false)) {
        return Result<ProcessOutput>::failure(execution_failure());
    }

    std::vector<unsigned char> attribute_storage;
    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    attribute_storage.resize(attribute_size);
    auto* attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(
        attribute_storage.data());
    if (InitializeProcThreadAttributeList(attributes, 1, 0,
                                          &attribute_size) == FALSE) {
        return Result<ProcessOutput>::failure(execution_failure());
    }
    HANDLE inherited[] = {stdin_read.get(), stdout_write.get(),
                          stderr_write.get()};
    if (UpdateProcThreadAttribute(
            attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
            sizeof(inherited), nullptr, nullptr) == FALSE) {
        DeleteProcThreadAttributeList(attributes);
        return Result<ProcessOutput>::failure(execution_failure());
    }

    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
        DeleteProcThreadAttributeList(attributes);
        return Result<ProcessOutput>::failure(execution_failure());
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits{};
    job_limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                                &job_limits, sizeof(job_limits)) == FALSE) {
        DeleteProcThreadAttributeList(attributes);
        return Result<ProcessOutput>::failure(execution_failure());
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = stdin_read.get();
    startup.StartupInfo.hStdOutput = stdout_write.get();
    startup.StartupInfo.hStdError = stderr_write.get();
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    const auto started_at = std::chrono::steady_clock::now();
    const BOOL created = CreateProcessW(
        nullptr, command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED |
            EXTENDED_STARTUPINFO_PRESENT,
        environment->data(), wide_cwd->c_str(), &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(attributes);
    if (created == FALSE) {
        return Result<ProcessOutput>::failure(start_failure());
    }
    UniqueHandle process_handle(process.hProcess);
    UniqueHandle thread_handle(process.hThread);
    if (AssignProcessToJobObject(job.get(), process_handle.get()) == FALSE ||
        ResumeThread(thread_handle.get()) == static_cast<DWORD>(-1)) {
        TerminateProcess(process_handle.get(), 1);
        return Result<ProcessOutput>::failure(execution_failure());
    }
    thread_handle.reset();
    stdin_read.reset();
    stdout_write.reset();
    stderr_write.reset();

    BoundedCollector stdout_collector(request.max_stdout_bytes);
    BoundedCollector stderr_collector(request.max_stderr_bytes);
    std::thread stdout_thread(
        [&] { read_handle(stdout_read.get(), stdout_collector); });
    std::thread stderr_thread(
        [&] { read_handle(stderr_read.get(), stderr_collector); });
    std::thread stdin_thread(write_handle, stdin_write.release(),
                             std::cref(request.stdin_utf8));

    const DWORD wait = WaitForSingleObject(
        process_handle.get(), static_cast<DWORD>(request.timeout_ms));
    const bool timed_out = wait == WAIT_TIMEOUT;
    if (timed_out) {
        TerminateJobObject(job.get(), 1);
        WaitForSingleObject(process_handle.get(), 5'000);
    }
    DWORD exit_code = static_cast<DWORD>(-1);
    if (wait == WAIT_FAILED ||
        GetExitCodeProcess(process_handle.get(), &exit_code) == FALSE) {
        TerminateJobObject(job.get(), 1);
        job.reset();
        stdin_thread.join();
        stdout_thread.join();
        stderr_thread.join();
        return Result<ProcessOutput>::failure(execution_failure());
    }
    TerminateJobObject(job.get(), exit_code);
    job.reset();
    stdin_thread.join();
    stdout_thread.join();
    stderr_thread.join();
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at);

    ProcessOutput output;
    output.exit_code =
        timed_out
            ? -1
            : static_cast<std::int64_t>(static_cast<std::int32_t>(exit_code));
    output.timed_out = timed_out;
    output.duration_ms = duration.count();
    auto normalized_stdout = normalized_utf8(stdout_collector.bytes(),
                                             request.max_stdout_bytes);
    auto normalized_stderr = normalized_utf8(stderr_collector.bytes(),
                                             request.max_stderr_bytes);
    output.stdout_utf8 = std::move(normalized_stdout.text);
    output.stderr_utf8 = std::move(normalized_stderr.text);
    output.stdout_truncated =
        stdout_collector.truncated() || normalized_stdout.truncated;
    output.stderr_truncated =
        stderr_collector.truncated() || normalized_stderr.truncated;
    return Result<ProcessOutput>::success(std::move(output));
}

#else

class UniqueFd final {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}
    ~UniqueFd() { reset(); }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }
    int get() const noexcept { return fd_; }
    int release() noexcept {
        const int value = fd_;
        fd_ = -1;
        return value;
    }
    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_{-1};
};

bool make_pipe(UniqueFd& read, UniqueFd& write) {
    int values[2];
    if (pipe(values) != 0) {
        return false;
    }
    read.reset(values[0]);
    write.reset(values[1]);
    for (const int fd : values) {
        const int flags = fcntl(fd, F_GETFD);
        if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
            return false;
        }
    }
    return true;
}

void read_fd(int fd, BoundedCollector& output) {
    char buffer[4096];
    for (;;) {
        const ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count <= 0) {
            break;
        }
        output.append(buffer, static_cast<std::size_t>(count));
    }
}

void write_fd(UniqueFd fd, const std::string& input) {
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
    std::size_t offset = 0;
    while (offset < input.size()) {
        const ssize_t count = write(fd.get(), input.data() + offset,
                                    input.size() - offset);
        if (count <= 0) {
            break;
        }
        offset += static_cast<std::size_t>(count);
    }
}

std::vector<std::pair<std::string, std::string>> safe_environment(
    const ProcessRequest& request) {
    static constexpr const char* kAllowed[] = {
        "PATH", "HOME", "USER", "LOGNAME", "SHELL", "TMPDIR", "TEMP",
        "TMP", "LANG", "LC_ALL", "CC", "CXX", "CMAKE_GENERATOR",
        "CMAKE_GENERATOR_PLATFORM", "CMAKE_GENERATOR_TOOLSET"};
    std::map<std::string, std::string> values;
    for (const auto* name : kAllowed) {
        const char* value = std::getenv(name);
        if (value != nullptr) {
            values.emplace(name, value);
        }
    }
    for (const auto& entry : request.environment_overrides) {
        if (sensitive_environment_name(entry.first)) {
            continue;
        }
        values[entry.first] = entry.second;
    }
    return {values.begin(), values.end()};
}

std::optional<std::string> resolved_executable(
    const ProcessRequest& request,
    const std::vector<std::pair<std::string, std::string>>& environment) {
    const auto executable = std::filesystem::u8path(request.program);
    const auto executable_name = executable.filename();
    if (executable_name.empty()) {
        return std::nullopt;
    }
    const auto usable = [](const std::filesystem::path& candidate) {
        return access(candidate.c_str(), X_OK) == 0;
    };
    if (request.program.find('/') != std::string::npos) {
        const auto candidate =
            (executable.is_absolute()
                 ? executable
                 : request.working_directory / executable)
                .lexically_normal();
        return usable(candidate)
                   ? std::optional<std::string>(candidate.generic_u8string())
                   : std::nullopt;
    }

    std::string path_value = "/usr/local/bin:/usr/bin:/bin";
    const auto found = std::find_if(
        environment.begin(), environment.end(),
        [](const auto& entry) { return entry.first == "PATH"; });
    if (found != environment.end()) {
        path_value = found->second;
    }
    std::size_t offset = 0;
    for (;;) {
        const auto delimiter = path_value.find(':', offset);
        const auto length = delimiter == std::string::npos
                                ? std::string::npos
                                : delimiter - offset;
        const auto entry = path_value.substr(offset, length);
        auto directory = entry.empty() ? request.working_directory
                                       : std::filesystem::u8path(entry);
        if (directory.is_relative()) {
            directory = request.working_directory / directory;
        }
        const auto candidate =
            (directory / executable_name).lexically_normal();
        if (usable(candidate)) {
            return candidate.generic_u8string();
        }
        if (delimiter == std::string::npos) {
            break;
        }
        offset = delimiter + 1;
    }
    return std::nullopt;
}

Result<ProcessOutput> run_native(const ProcessRequest& request,
                          const SandboxedCommand& wrapped) {
    UniqueFd stdout_read, stdout_write;
    UniqueFd stderr_read, stderr_write;
    UniqueFd stdin_read, stdin_write;
    UniqueFd exec_read, exec_write;
    if (!make_pipe(stdout_read, stdout_write) ||
        !make_pipe(stderr_read, stderr_write) ||
        !make_pipe(stdin_read, stdin_write) ||
        !make_pipe(exec_read, exec_write)) {
        return Result<ProcessOutput>::failure(execution_failure());
    }
    const auto environment = safe_environment(request);
    const auto executable = resolved_executable(request, environment);
    if (!executable.has_value()) {
        return Result<ProcessOutput>::failure(start_failure());
    }
    std::vector<std::string> environment_storage;
    environment_storage.reserve(environment.size());
    for (const auto& entry : environment) {
        environment_storage.push_back(entry.first + "=" + entry.second);
    }
    std::vector<char*> environment_pointers;
    environment_pointers.reserve(environment_storage.size() + 1);
    for (auto& entry : environment_storage) {
        environment_pointers.push_back(entry.data());
    }
    environment_pointers.push_back(nullptr);

    std::vector<std::string> argument_storage;
    argument_storage.reserve(wrapped.arguments.size() + 1);
    if (!wrapped.program.empty()) {
        argument_storage.push_back(wrapped.program);
    } else {
        argument_storage.push_back(request.program);
    }
    for (const auto& argument : wrapped.arguments) {
        argument_storage.push_back(argument);
    }
    if (wrapped.arguments.empty()) {
        for (const auto& argument : request.arguments) {
            argument_storage.push_back(argument);
        }
    }
    std::vector<std::string> argument_storage;
    argument_storage.reserve(wrapped.arguments.size() + 1);
    argument_storage.push_back(wrapped.program);
    for (const auto& argument : wrapped.arguments) {
        argument_storage.push_back(argument);
    }
    std::vector<char*> argument_pointers;
    argument_pointers.reserve(argument_storage.size() + 1);
    for (auto& entry : argument_storage) {
        argument_pointers.push_back(entry.data());
    }
    argument_pointers.push_back(nullptr);

    const auto started_at = std::chrono::steady_clock::now();
    const pid_t child = fork();
    if (child < 0) {
        return Result<ProcessOutput>::failure(start_failure());
    }
    if (child == 0) {
        const auto child_failure = [&](int exit_code) {
            const int code = 1;
            const auto ignored = write(exec_write.get(), &code, sizeof(code));
            static_cast<void>(ignored);
            _exit(exit_code);
        };
        if (setpgid(0, 0) != 0 ||
            dup2(stdin_read.get(), STDIN_FILENO) < 0 ||
            dup2(stdout_write.get(), STDOUT_FILENO) < 0 ||
            dup2(stderr_write.get(), STDERR_FILENO) < 0) {
            child_failure(126);
        }
        const int descriptors[] = {
            stdin_read.get(),  stdin_write.get(), stdout_read.get(),
            stdout_write.get(), stderr_read.get(), stderr_write.get(),
            exec_read.get()};
        for (const int descriptor : descriptors) {
            if (descriptor > STDERR_FILENO) {
                close(descriptor);
            }
        }
        if (chdir(request.working_directory.c_str()) != 0) {
            child_failure(126);
        }
        execve(executable->c_str(), argument_pointers.data(),
               environment_pointers.data());
        child_failure(127);
    }
    if (setpgid(child, child) != 0 && errno != EACCES && errno != ESRCH) {
        kill(child, SIGKILL);
        waitpid(child, nullptr, 0);
        return Result<ProcessOutput>::failure(start_failure());
    }
    stdin_read.reset();
    stdout_write.reset();
    stderr_write.reset();
    exec_write.reset();
    int exec_code = 0;
    const ssize_t exec_result = read(exec_read.get(), &exec_code,
                                     sizeof(exec_code));
    exec_read.reset();
    if (exec_result > 0) {
        kill(-child, SIGKILL);
        waitpid(child, nullptr, 0);
        return Result<ProcessOutput>::failure(start_failure());
    }

    BoundedCollector stdout_collector(request.max_stdout_bytes);
    BoundedCollector stderr_collector(request.max_stderr_bytes);
    std::thread stdout_thread(
        [&] { read_fd(stdout_read.get(), stdout_collector); });
    std::thread stderr_thread(
        [&] { read_fd(stderr_read.get(), stderr_collector); });
    std::thread stdin_thread(write_fd, std::move(stdin_write),
                             std::cref(request.stdin_utf8));

    int status = 0;
    bool timed_out = false;
    for (;;) {
        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) {
            break;
        }
        if (waited < 0) {
            kill(-child, SIGKILL);
            stdin_thread.join();
            stdout_thread.join();
            stderr_thread.join();
            return Result<ProcessOutput>::failure(execution_failure());
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at);
        if (elapsed.count() >= request.timeout_ms) {
            timed_out = true;
            kill(-child, SIGKILL);
            waitpid(child, &status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    kill(-child, SIGKILL);
    stdin_thread.join();
    stdout_thread.join();
    stderr_thread.join();

    std::int64_t exit_code = -1;
    if (!timed_out && WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
    } else if (!timed_out && WIFSIGNALED(status)) {
        exit_code = 128 + WTERMSIG(status);
    }
    const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started_at);
    ProcessOutput output;
    output.exit_code = exit_code;
    output.timed_out = timed_out;
    output.duration_ms = duration.count();
    auto normalized_stdout = normalized_utf8(stdout_collector.bytes(),
                                             request.max_stdout_bytes);
    auto normalized_stderr = normalized_utf8(stderr_collector.bytes(),
                                             request.max_stderr_bytes);
    output.stdout_utf8 = std::move(normalized_stdout.text);
    output.stderr_utf8 = std::move(normalized_stderr.text);
    output.stdout_truncated =
        stdout_collector.truncated() || normalized_stdout.truncated;
    output.stderr_truncated =
        stderr_collector.truncated() || normalized_stderr.truncated;
    return Result<ProcessOutput>::success(std::move(output));
}

#endif

}  // namespace

DirectProcessRunner::DirectProcessRunner() = default;

DirectProcessRunner::DirectProcessRunner(std::shared_ptr<Sandbox> sandbox)
    : sandbox_(std::move(sandbox)) {}

Result<ProcessOutput> DirectProcessRunner::run(const ProcessRequest& request) {
    if (!valid_request(request)) {
        return Result<ProcessOutput>::failure(invalid_request());
    }
    // T22: when a sandbox is attached, translate the request into a
    // SandboxedCommand before any host execution. When sandbox_ is null
    // we still populate wrapped with the passthrough command so the
    // platform runner reads a single, well-formed argv regardless of
    // whether sandboxing is active.
    SandboxedCommand wrapped;
    if (sandbox_) {
        const SandboxProfile default_profile{};
        if (auto fault = sandbox_->apply(
                default_profile, request.program, request.arguments,
                request.working_directory.generic_u8string(), wrapped);
            fault.has_value()) {
            return Result<ProcessOutput>::failure(
                {ErrorCode::DependencyUnavailable, fault->message, false});
        }
    } else {
        wrapped.program = request.program;
        wrapped.arguments = request.arguments;
    }
    try {
        return run_native(request, wrapped);
    } catch (...) {
        return Result<ProcessOutput>::failure(execution_failure());
    }
}

}  // namespace agent

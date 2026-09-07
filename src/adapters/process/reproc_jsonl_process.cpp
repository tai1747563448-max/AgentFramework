#include "adapters/process/reproc_jsonl_process.h"

#include "adapters/workspace/workspace_text.h"

#include <reproc++/reproc.hpp>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace agent {
namespace {

RuntimeError invalid_input() {
    return {ErrorCode::InvalidInput, "invalid JSONL process request", false};
}

RuntimeError unavailable() {
    return {ErrorCode::DependencyUnavailable,
            "JSONL process is unavailable", true};
}

RuntimeError timeout_error() {
    return {ErrorCode::RequestTimeout, "JSONL process timed out", true};
}

RuntimeError protocol_error() {
    return {ErrorCode::ProtocolFailure, "JSONL process protocol failed", false};
}

bool valid_line(const std::string& line, std::size_t maximum) {
    return !line.empty() && line.size() <= maximum &&
           line.find('\0') == std::string::npos &&
           line.find('\r') == std::string::npos &&
           line.find('\n') == std::string::npos &&
           workspace::is_strict_utf8_text(line);
}

bool valid_start_request(const JsonlProcessRequest& request) {
    if (request.program.empty() ||
        !workspace::is_strict_utf8_text(request.program) ||
        request.program.find('\0') != std::string::npos ||
        request.working_directory.empty() ||
        request.startup_timeout_ms <= 0 ||
        request.startup_timeout_ms > 600'000 ||
        request.max_stdout_line_bytes == 0 ||
        request.max_stdout_line_bytes > 1024U * 1024U ||
        request.max_stderr_bytes == 0 ||
        request.max_stderr_bytes > 1024U * 1024U) {
        return false;
    }
    std::error_code error;
    if (!std::filesystem::is_directory(request.working_directory, error) ||
        error) {
        return false;
    }
    return std::all_of(request.arguments.begin(), request.arguments.end(),
                       [](const std::string& argument) {
                           return argument.find('\0') == std::string::npos &&
                                  workspace::is_strict_utf8_text(argument);
                       });
}

std::map<std::string, std::string> safe_environment() {
    static constexpr const char* names[] = {
        "PATH",          "PATHEXT",      "SYSTEMROOT", "WINDIR",
        "COMSPEC",       "TEMP",         "TMP",        "USERPROFILE",
        "HOMEDRIVE",     "HOMEPATH",     "APPDATA",    "LOCALAPPDATA",
        "PROGRAMDATA",   "PROGRAMFILES", "NUMBER_OF_PROCESSORS",
        "PROCESSOR_ARCHITECTURE",          "OS",         "SYSTEMDRIVE"};
    std::map<std::string, std::string> result;
    for (const auto* name : names) {
        const char* value = std::getenv(name);
        if (value != nullptr) {
            result.emplace(name, value);
        }
    }
    return result;
}

}  // namespace

class ReprocJsonlProcess::Impl final {
public:
    ~Impl() { stop_noexcept(500); }

    Result<std::string> start(const JsonlProcessRequest& request) {
        std::lock_guard<std::mutex> operation(operation_mutex_);
        if (!valid_start_request(request) || running_unlocked()) {
            return Result<std::string>::failure(invalid_input());
        }
        cleanup_reaped();
        max_line_bytes_ = request.max_stdout_line_bytes;
        max_stderr_bytes_ = request.max_stderr_bytes;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            stdout_buffer_.clear();
            stdout_lines_.clear();
            outbound_lines_.clear();
            outbound_offset_ = 0;
            stderr_bytes_.clear();
            stderr_truncated_ = false;
            protocol_failed_ = false;
            exited_ = false;
            started_ = false;
        }
        reader_stop_.store(false);
        std::vector<std::string> arguments;
        arguments.reserve(request.arguments.size() + 1);
        arguments.push_back(request.program);
        arguments.insert(arguments.end(), request.arguments.begin(),
                         request.arguments.end());
        const auto cwd = request.working_directory.generic_u8string();
        auto environment = safe_environment();
        reproc::options options;
        options.working_directory = cwd.c_str();
        options.env.behavior = reproc::env::empty;
        options.env.extra = environment;
        options.redirect.in.type = reproc::redirect::pipe;
        options.redirect.out.type = reproc::redirect::pipe;
        options.redirect.err.type = reproc::redirect::pipe;
        options.stop = {{reproc::stop::kill, reproc::milliseconds(1'000)},
                        {}, {}};
        process_ = reproc::process();
        const auto start_error = process_.start(arguments, options);
        if (start_error) {
            return Result<std::string>::failure(unavailable());
        }
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            started_ = true;
        }
        if (!attach_job()) {
            force_stop();
            return Result<std::string>::failure(unavailable());
        }
        reader_ = std::thread([this] { reader_loop(); });
        const auto ready = wait_line(request.startup_timeout_ms);
        if (!ready.has_value()) {
            force_stop();
        }
        return ready;
    }

    Result<std::string> exchange(const std::string& line,
                                 std::int64_t timeout_ms) {
        std::lock_guard<std::mutex> operation(operation_mutex_);
        if (!valid_line(line, max_line_bytes_) || timeout_ms <= 0 ||
            timeout_ms > 600'000) {
            return Result<std::string>::failure(invalid_input());
        }
        if (!running_unlocked()) {
            return Result<std::string>::failure(unavailable());
        }
        std::string framed = line;
        framed.push_back('\n');
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            outbound_lines_.push_back(std::move(framed));
        }
        const auto response = wait_line(timeout_ms);
        if (!response.has_value()) {
            force_stop();
        }
        return response;
    }

    bool running() const noexcept {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return started_ && !exited_ && !protocol_failed_;
    }

    Result<void> stop(std::int64_t timeout_ms) {
        std::lock_guard<std::mutex> operation(operation_mutex_);
        if (timeout_ms <= 0 || timeout_ms > 600'000) {
            return Result<void>::failure(invalid_input());
        }
        if (!started_snapshot()) {
            return Result<void>::success();
        }
        stop_reader();
        process_.close(reproc::stream::in);
        const auto actions = reproc::stop_actions{
            {reproc::stop::wait, reproc::milliseconds(
                                     static_cast<int>(timeout_ms))},
            {reproc::stop::kill, reproc::milliseconds(1'000)}, {}};
        const auto result = process_.stop(actions);
        close_job();
        mark_stopped();
        if (result.second) {
            return Result<void>::failure(unavailable());
        }
        return Result<void>::success();
    }

    std::string stderr_tail() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return {stderr_bytes_.begin(), stderr_bytes_.end()};
    }

    bool stderr_truncated() const noexcept {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return stderr_truncated_;
    }

private:
    Result<std::string> wait_line(std::int64_t timeout_ms) {
        std::unique_lock<std::mutex> lock(state_mutex_);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        if (!state_changed_.wait_until(lock, deadline, [this] {
                return !stdout_lines_.empty() || protocol_failed_ || exited_;
            })) {
            return Result<std::string>::failure(timeout_error());
        }
        if (!stdout_lines_.empty()) {
            auto line = std::move(stdout_lines_.front());
            stdout_lines_.pop_front();
            return Result<std::string>::success(std::move(line));
        }
        if (protocol_failed_) {
            return Result<std::string>::failure(protocol_error());
        }
        return Result<std::string>::failure(unavailable());
    }

    void append_stdout(const std::uint8_t* bytes, std::size_t size) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (std::size_t index = 0; index < size; ++index) {
            const char byte = static_cast<char>(bytes[index]);
            if (byte == '\0') {
                protocol_failed_ = true;
                state_changed_.notify_all();
                return;
            }
            if (byte == '\n') {
                if (!stdout_buffer_.empty() && stdout_buffer_.back() == '\r') {
                    stdout_buffer_.pop_back();
                }
                if (!valid_line(stdout_buffer_, max_line_bytes_)) {
                    protocol_failed_ = true;
                    state_changed_.notify_all();
                    return;
                }
                stdout_lines_.push_back(std::move(stdout_buffer_));
                stdout_buffer_.clear();
                state_changed_.notify_all();
                continue;
            }
            if (stdout_buffer_.size() >= max_line_bytes_) {
                protocol_failed_ = true;
                state_changed_.notify_all();
                return;
            }
            stdout_buffer_.push_back(byte);
        }
    }

    void append_stderr(const std::uint8_t* bytes, std::size_t size) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (std::size_t index = 0; index < size; ++index) {
            if (stderr_bytes_.size() == max_stderr_bytes_) {
                stderr_bytes_.pop_front();
                stderr_truncated_ = true;
            }
            stderr_bytes_.push_back(static_cast<char>(bytes[index]));
        }
    }

    void reader_loop() {
        std::uint8_t buffer[4096]{};
        while (!reader_stop_.load()) {
            bool did_work = false;
            const auto polled = process_.poll(
                reproc::event::in | reproc::event::out | reproc::event::err |
                    reproc::event::exit,
                reproc::milliseconds(50));
            if (polled.second) {
                if (polled.second == std::errc::timed_out) {
                    continue;
                }
                std::lock_guard<std::mutex> lock(state_mutex_);
                exited_ = true;
                state_changed_.notify_all();
                break;
            }
            if ((polled.first & reproc::event::err) != 0) {
                const auto read =
                    process_.read(reproc::stream::err, buffer, sizeof(buffer));
                if (!read.second) {
                    append_stderr(buffer, read.first);
                    did_work = did_work || read.first != 0;
                }
            }
            if ((polled.first & reproc::event::out) != 0) {
                const auto read =
                    process_.read(reproc::stream::out, buffer, sizeof(buffer));
                if (!read.second) {
                    append_stdout(buffer, read.first);
                    did_work = read.first != 0;
                }
            }
            if ((polled.first & reproc::event::in) != 0) {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (!outbound_lines_.empty()) {
                    const auto& pending = outbound_lines_.front();
                    const auto written = process_.write(
                        reinterpret_cast<const std::uint8_t*>(pending.data() +
                                                              outbound_offset_),
                        pending.size() - outbound_offset_);
                    if (written.second || written.first == 0) {
                        exited_ = true;
                        state_changed_.notify_all();
                        break;
                    }
                    outbound_offset_ += written.first;
                    if (outbound_offset_ == pending.size()) {
                        outbound_lines_.pop_front();
                        outbound_offset_ = 0;
                    }
                    did_work = true;
                }
            }
            if ((polled.first & reproc::event::exit) != 0) {
                std::lock_guard<std::mutex> lock(state_mutex_);
                exited_ = true;
                state_changed_.notify_all();
                break;
            }
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (protocol_failed_) {
                    break;
                }
            }
            if (!did_work) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    }

    bool running_unlocked() const noexcept { return running(); }

    bool started_snapshot() const noexcept {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return started_;
    }

    void stop_reader() noexcept {
        reader_stop_.store(true);
        if (reader_.joinable()) {
            reader_.join();
        }
    }

    void mark_stopped() noexcept {
        std::lock_guard<std::mutex> lock(state_mutex_);
        started_ = false;
        exited_ = true;
    }

    void force_stop() noexcept {
        if (!started_snapshot()) {
            return;
        }
        reader_stop_.store(true);
        stop_reader();
        process_.close(reproc::stream::in);
        process_.stop({{reproc::stop::kill, reproc::milliseconds(1'000)},
                       {}, {}});
        close_job();
        mark_stopped();
    }

    void cleanup_reaped() noexcept {
        if (started_snapshot()) {
            force_stop();
        }
        stop_reader();
        close_job();
    }

    void stop_noexcept(std::int64_t timeout_ms) noexcept {
        std::lock_guard<std::mutex> operation(operation_mutex_);
        if (!started_snapshot()) {
            stop_reader();
            close_job();
            return;
        }
        stop_reader();
        process_.close(reproc::stream::in);
        process_.stop({{reproc::stop::wait,
                        reproc::milliseconds(static_cast<int>(timeout_ms))},
                       {reproc::stop::kill, reproc::milliseconds(1'000)}, {}});
        close_job();
        mark_stopped();
    }

    bool attach_job() noexcept {
#if defined(_WIN32)
        job_ = CreateJobObjectW(nullptr, nullptr);
        if (job_ == nullptr) {
            return false;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(job_, JobObjectExtendedLimitInformation,
                                    &limits, sizeof(limits)) == FALSE) {
            close_job();
            return false;
        }
        const auto pid = process_.pid();
        if (pid.second || pid.first <= 0) {
            close_job();
            return false;
        }
        HANDLE child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE |
                                       PROCESS_QUERY_LIMITED_INFORMATION,
                                   FALSE, static_cast<DWORD>(pid.first));
        if (child == nullptr) {
            close_job();
            return false;
        }
        const BOOL assigned = AssignProcessToJobObject(job_, child);
        CloseHandle(child);
        if (assigned == FALSE) {
            close_job();
            return false;
        }
#endif
        return true;
    }

    void close_job() noexcept {
#if defined(_WIN32)
        if (job_ != nullptr) {
            CloseHandle(job_);
            job_ = nullptr;
        }
#endif
    }

    mutable std::mutex state_mutex_;
    std::mutex operation_mutex_;
    std::condition_variable state_changed_;
    reproc::process process_;
    std::thread reader_;
    std::atomic<bool> reader_stop_{false};
    bool started_{false};
    bool exited_{false};
    bool protocol_failed_{false};
    std::size_t max_line_bytes_{65'536};
    std::size_t max_stderr_bytes_{4'096};
    std::string stdout_buffer_;
    std::deque<std::string> stdout_lines_;
    std::deque<std::string> outbound_lines_;
    std::size_t outbound_offset_{0};
    std::deque<char> stderr_bytes_;
    bool stderr_truncated_{false};
#if defined(_WIN32)
    HANDLE job_{nullptr};
#endif
};

ReprocJsonlProcess::ReprocJsonlProcess() : impl_(std::make_unique<Impl>()) {}

ReprocJsonlProcess::~ReprocJsonlProcess() = default;

Result<std::string> ReprocJsonlProcess::start(
    const JsonlProcessRequest& request) {
    return impl_->start(request);
}

Result<std::string> ReprocJsonlProcess::exchange(const std::string& line,
                                                 std::int64_t timeout_ms) {
    return impl_->exchange(line, timeout_ms);
}

bool ReprocJsonlProcess::running() const noexcept { return impl_->running(); }

Result<void> ReprocJsonlProcess::stop(std::int64_t timeout_ms) {
    return impl_->stop(timeout_ms);
}

std::string ReprocJsonlProcess::stderr_tail() const {
    return impl_->stderr_tail();
}

bool ReprocJsonlProcess::stderr_truncated() const noexcept {
    return impl_->stderr_truncated();
}

}  // namespace agent

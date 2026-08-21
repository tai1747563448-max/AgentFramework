#include "cli/cli_app.h"

#include <filesystem>
#include <cwchar>
#include <limits>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace agent {
namespace {

Result<StartupArguments> invalid_startup_arguments(const char* message) {
    return Result<StartupArguments>::failure(
        {ErrorCode::InvalidInput, message, false});
}

const char* status_name(TaskStatus status) {
    switch (status) {
        case TaskStatus::Created:
            return "Created";
        case TaskStatus::PreparingContext:
            return "PreparingContext";
        case TaskStatus::AwaitingModel:
            return "AwaitingModel";
        case TaskStatus::AwaitingTool:
            return "AwaitingTool";
        case TaskStatus::Completed:
            return "Completed";
        case TaskStatus::Failed:
            return "Failed";
        case TaskStatus::BudgetExceeded:
            return "BudgetExceeded";
        case TaskStatus::Cancelled:
            return "Cancelled";
    }
    return "Unknown";
}

const char* event_kind_name(EventKind event_kind) {
    switch (event_kind) {
        case EventKind::TaskStarted:
            return "TaskStarted";
        case EventKind::ContextPreparationStarted:
            return "ContextPreparationStarted";
        case EventKind::ContextPrepared:
            return "ContextPrepared";
        case EventKind::ContextPreparationFailed:
            return "ContextPreparationFailed";
        case EventKind::ModelCallStarted:
            return "ModelCallStarted";
        case EventKind::ModelCallSucceeded:
            return "ModelCallSucceeded";
        case EventKind::ModelCallFailed:
            return "ModelCallFailed";
        case EventKind::ToolCallStarted:
            return "ToolCallStarted";
        case EventKind::ToolCallSucceeded:
            return "ToolCallSucceeded";
        case EventKind::ToolCallFailed:
            return "ToolCallFailed";
        case EventKind::TaskCompleted:
            return "TaskCompleted";
        case EventKind::TaskFailed:
            return "TaskFailed";
        case EventKind::TaskBudgetExceeded:
            return "TaskBudgetExceeded";
        case EventKind::TaskCancelled:
            return "TaskCancelled";
    }
    return "Unknown";
}

const char* error_code_name(ErrorCode code) {
    switch (code) {
        case ErrorCode::InvalidInput:
            return "InvalidInput";
        case ErrorCode::InvalidConfiguration:
            return "InvalidConfiguration";
        case ErrorCode::PersistenceFailure:
            return "PersistenceFailure";
        case ErrorCode::TransportFailure:
            return "TransportFailure";
        case ErrorCode::RequestTimeout:
            return "RequestTimeout";
        case ErrorCode::HttpFailure:
            return "HttpFailure";
        case ErrorCode::ProtocolFailure:
            return "ProtocolFailure";
        case ErrorCode::DependencyUnavailable:
            return "DependencyUnavailable";
        case ErrorCode::InvalidTransition:
            return "InvalidTransition";
        case ErrorCode::BudgetExceeded:
            return "BudgetExceeded";
        case ErrorCode::Cancelled:
            return "Cancelled";
    }
    return "Unknown";
}

const char* terminal_summary(TaskStatus status) {
    switch (status) {
        case TaskStatus::Failed:
            return "task failed; inspect the local event log";
        case TaskStatus::BudgetExceeded:
            return "task budget exceeded; adjust limits before retrying";
        case TaskStatus::Cancelled:
            return "task cancelled; rerun when ready";
        default:
            return "runtime returned an unexpected terminal state";
    }
}

const char* fatal_summary(ErrorCode code) {
    if (code == ErrorCode::PersistenceFailure) {
        return "runtime persistence failed; inspect local storage before retrying";
    }
    return "runtime failed; inspect the local event log";
}

int exit_for_error(ErrorCode code) {
    switch (code) {
        case ErrorCode::InvalidInput:
        case ErrorCode::InvalidConfiguration:
            return ExitCode::InvalidInputOrConfig;
        case ErrorCode::PersistenceFailure:
            return ExitCode::PersistenceFailure;
        case ErrorCode::BudgetExceeded:
            return ExitCode::BudgetExceeded;
        case ErrorCode::Cancelled:
            return ExitCode::Cancelled;
        default:
            return ExitCode::TaskFailed;
    }
}

bool valid_generated_task_id(std::string_view task_id) {
    return is_valid_task_id(task_id);
}

char hex_digit(unsigned int value) {
    return "0123456789ABCDEF"[value & 0x0FU];
}

std::string byte_escape(unsigned char byte) {
    std::string escaped{"\\x00"};
    escaped[2] = hex_digit(byte >> 4U);
    escaped[3] = hex_digit(byte);
    return escaped;
}

std::string control_escape(std::uint32_t code_point) {
    std::string escaped{"\\u0000"};
    escaped[4] = hex_digit(code_point >> 4U);
    escaped[5] = hex_digit(code_point);
    return escaped;
}

bool continuation(unsigned char byte) {
    return byte >= 0x80U && byte <= 0xBFU;
}

struct Utf8Unit {
    bool valid{false};
    std::size_t length{1};
    std::uint32_t code_point{0};
};

Utf8Unit decode_utf8_unit(std::string_view text, std::size_t offset) {
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first <= 0x7FU) {
        return {true, 1, first};
    }
    if (first >= 0xC2U && first <= 0xDFU && offset + 1 < text.size()) {
        const auto second = static_cast<unsigned char>(text[offset + 1]);
        if (continuation(second)) {
            return {true, 2,
                    static_cast<std::uint32_t>(((first & 0x1FU) << 6U) |
                                               (second & 0x3FU))};
        }
    }
    if (first >= 0xE0U && first <= 0xEFU && offset + 2 < text.size()) {
        const auto second = static_cast<unsigned char>(text[offset + 1]);
        const auto third = static_cast<unsigned char>(text[offset + 2]);
        const bool valid_second =
            continuation(second) &&
            (first != 0xE0U || second >= 0xA0U) &&
            (first != 0xEDU || second <= 0x9FU);
        if (valid_second && continuation(third)) {
            return {true, 3,
                    static_cast<std::uint32_t>(((first & 0x0FU) << 12U) |
                                               ((second & 0x3FU) << 6U) |
                                               (third & 0x3FU))};
        }
    }
    if (first >= 0xF0U && first <= 0xF4U && offset + 3 < text.size()) {
        const auto second = static_cast<unsigned char>(text[offset + 1]);
        const auto third = static_cast<unsigned char>(text[offset + 2]);
        const auto fourth = static_cast<unsigned char>(text[offset + 3]);
        const bool valid_second =
            continuation(second) &&
            (first != 0xF0U || second >= 0x90U) &&
            (first != 0xF4U || second <= 0x8FU);
        if (valid_second && continuation(third) && continuation(fourth)) {
            return {true, 4,
                    static_cast<std::uint32_t>(((first & 0x07U) << 18U) |
                                               ((second & 0x3FU) << 12U) |
                                               ((third & 0x3FU) << 6U) |
                                               (fourth & 0x3FU))};
        }
    }
    return {};
}

std::string render_terminal_text(std::string_view text) {
    constexpr std::size_t kRenderedLimit = 8192;
    constexpr std::string_view kTruncationMarker = "\n[output truncated]";
    std::string rendered;
    rendered.reserve(text.size() < kRenderedLimit ? text.size()
                                                   : kRenderedLimit);
    bool truncated = false;
    for (std::size_t offset = 0; offset < text.size();) {
        const auto unit = decode_utf8_unit(text, offset);
        std::string escaped;
        std::string_view token;
        if (!unit.valid) {
            escaped = byte_escape(static_cast<unsigned char>(text[offset]));
            token = escaped;
        } else if ((unit.code_point < 0x20U && unit.code_point != '\n' &&
                    unit.code_point != '\t') ||
                   unit.code_point == 0x7FU ||
                   (unit.code_point >= 0x80U && unit.code_point <= 0x9FU)) {
            escaped = control_escape(unit.code_point);
            token = escaped;
        } else {
            token = text.substr(offset, unit.length);
        }
        if (rendered.size() + token.size() > kRenderedLimit) {
            truncated = true;
            break;
        }
        rendered.append(token.data(), token.size());
        offset += unit.valid ? unit.length : 1;
    }
    if (truncated) {
        rendered.append(kTruncationMarker.data(), kTruncationMarker.size());
    }
    return rendered;
}

int render_runtime_result(const RuntimeResult& result,
                          bool progress_valid,
                          std::ostream& output,
                          std::ostream& error) {
    if (!progress_valid) {
        error << "runtime progress contained an invalid task ID\n";
        return ExitCode::TaskFailed;
    }
    if (result.fatal_error.has_value()) {
        if (result.state.has_value()) {
            if (!valid_generated_task_id(result.state->task_id)) {
                error << "runtime returned an invalid task ID\n";
                return ExitCode::TaskFailed;
            }
            error << "task_id=" << result.state->task_id
                  << " status=" << status_name(result.state->status)
                  << " error_code="
                  << error_code_name(result.fatal_error->code)
                  << " summary=" << fatal_summary(result.fatal_error->code)
                  << '\n';
        } else {
            error << "error_code=" << error_code_name(result.fatal_error->code)
                  << " summary=runtime failed before a durable task state was "
                     "available\n";
        }
        return exit_for_error(result.fatal_error->code);
    }
    if (!result.state.has_value()) {
        error << "runtime returned no task state\n";
        return ExitCode::TaskFailed;
    }
    if (!valid_generated_task_id(result.state->task_id)) {
        error << "runtime returned an invalid task ID\n";
        return ExitCode::TaskFailed;
    }

    switch (result.state->status) {
        case TaskStatus::Completed:
            if (!result.state->final_text.has_value()) {
                error << "completed task has no final output\n";
                return ExitCode::TaskFailed;
            }
            output << render_terminal_text(*result.state->final_text) << '\n';
            return ExitCode::Success;
        case TaskStatus::BudgetExceeded:
            error << "task_id=" << result.state->task_id
                  << " status=" << status_name(result.state->status)
                  << " error_code="
                  << (result.state->terminal_error.has_value()
                          ? error_code_name(result.state->terminal_error->code)
                          : "Unknown")
                  << " summary=" << terminal_summary(result.state->status)
                  << '\n';
            return ExitCode::BudgetExceeded;
        case TaskStatus::Cancelled:
            error << "task_id=" << result.state->task_id
                  << " status=" << status_name(result.state->status)
                  << " error_code="
                  << (result.state->terminal_error.has_value()
                          ? error_code_name(result.state->terminal_error->code)
                          : "Unknown")
                  << " summary=" << terminal_summary(result.state->status)
                  << '\n';
            return ExitCode::Cancelled;
        case TaskStatus::Failed:
            error << "task_id=" << result.state->task_id
                  << " status=" << status_name(result.state->status)
                  << " error_code="
                  << (result.state->terminal_error.has_value()
                          ? error_code_name(result.state->terminal_error->code)
                          : "Unknown")
                  << " summary=" << terminal_summary(result.state->status)
                  << '\n';
            return ExitCode::TaskFailed;
        default:
            error << "runtime returned a non-terminal task state\n";
            return ExitCode::TaskFailed;
    }
}

}  // namespace

Result<StartupArguments> parse_startup_arguments(
    const std::vector<std::string>& args) {
    StartupArguments parsed;
    parsed.command_args.reserve(args.size());
    if (args.empty()) {
        return invalid_startup_arguments("missing executable argument");
    }
    parsed.command_args.push_back(args.front());

    for (std::size_t index = 1; index < args.size(); ++index) {
        if (args[index] != "--env-file") {
            parsed.command_args.push_back(args[index]);
            continue;
        }
        if (parsed.env_file.has_value() || index + 1 >= args.size() ||
            args[index + 1].empty()) {
            return invalid_startup_arguments(
                "--env-file requires exactly one non-empty path");
        }
        std::error_code error;
        auto path = std::filesystem::absolute(
            std::filesystem::u8path(args[++index]), error);
        if (error) {
            return invalid_startup_arguments(
                "--env-file path could not be resolved");
        }
        parsed.env_file = path.lexically_normal();
    }
    if (parsed.env_file.has_value() && parsed.command_args.size() > 1 &&
        (parsed.command_args[1] == "verify-log" ||
         parsed.command_args[1] == "evaluate-log")) {
        return invalid_startup_arguments(
            "--env-file cannot be combined with credential-free commands");
    }
    return Result<StartupArguments>::success(std::move(parsed));
}

#if defined(_WIN32)
Result<std::vector<std::string>> utf8_arguments_from_windows(
    int argc,
    const wchar_t* const argv[]) {
    if (argc < 0 || (argc > 0 && argv == nullptr)) {
        return Result<std::vector<std::string>>::failure(
            {ErrorCode::InvalidInput, "invalid Windows command line", false});
    }

    std::vector<std::string> converted;
    converted.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        if (argv[index] == nullptr) {
            return Result<std::vector<std::string>>::failure(
                {ErrorCode::InvalidInput, "invalid Windows command line",
                 false});
        }
        const auto wide_length = std::wcslen(argv[index]);
        if (wide_length == 0) {
            converted.emplace_back();
            continue;
        }
        if (wide_length >
            static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return Result<std::vector<std::string>>::failure(
                {ErrorCode::InvalidInput, "Windows command line is too long",
                 false});
        }
        const int length = static_cast<int>(wide_length);
        const int byte_count = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, argv[index], length, nullptr, 0,
            nullptr, nullptr);
        if (byte_count <= 0) {
            return Result<std::vector<std::string>>::failure(
                {ErrorCode::InvalidInput,
                 "Windows command line contains invalid Unicode", false});
        }
        std::string utf8(static_cast<std::size_t>(byte_count), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[index],
                                length, utf8.data(), byte_count, nullptr,
                                nullptr) != byte_count) {
            return Result<std::vector<std::string>>::failure(
                {ErrorCode::InvalidInput,
                 "Windows command line contains invalid Unicode", false});
        }
        converted.push_back(std::move(utf8));
    }
    return Result<std::vector<std::string>>::success(std::move(converted));
}
#endif

CliApp::CliApp(RunCommand run,
               VerifyCommand verify,
               std::ostream& output,
               std::ostream& error)
    : CliApp(
          std::move(run),
          [](const std::string&,
             const RuntimeProgressObserver&) {
              return RuntimeResult{
                  std::nullopt,
                  RuntimeError{ErrorCode::InvalidInput,
                               "resume command is unavailable", false}};
          },
          std::move(verify), output, error) {}

CliApp::CliApp(RunCommand run,
               ResumeCommand resume,
               VerifyCommand verify,
               std::ostream& output,
               std::ostream& error)
    : CliApp(
          std::move(run), std::move(resume), std::move(verify),
          [](const std::filesystem::path&) {
              return Result<TaskEvaluation>::failure(
                  {ErrorCode::InvalidInput,
                   "evaluate command is unavailable", false});
          },
          output, error) {}

CliApp::CliApp(RunCommand run,
               ResumeCommand resume,
               VerifyCommand verify,
               EvaluateCommand evaluate,
               std::ostream& output,
               std::ostream& error)
    : run_(std::move(run)),
      resume_(std::move(resume)),
      verify_(std::move(verify)),
      evaluate_(std::move(evaluate)),
      output_(output),
      error_(error) {}

int CliApp::execute(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        error_ << "usage: agent run --workspace <path> --issue <text> | "
                  "agent resume --task-id <task-id> | "
                  "agent verify-log --events <path> | "
                  "agent evaluate-log --events <path>\n";
        return ExitCode::InvalidInputOrConfig;
    }

    if (args[1] == "verify-log") {
        if (args.size() != 4 || args[2] != "--events" || args[3].empty()) {
            error_ << "verify-log requires --events <event-log path>\n";
            return ExitCode::InvalidInputOrConfig;
        }
        const auto verified = verify_(std::filesystem::u8path(args[3]));
        if (!verified.has_value()) {
            error_ << "event log validation failed\n";
            return ExitCode::InvalidEventLog;
        }
        const auto& state = verified.value();
        if (!valid_generated_task_id(state.task_id)) {
            error_ << "event log contains an invalid task ID\n";
            return ExitCode::InvalidEventLog;
        }
        output_ << "task_id=" << state.task_id << " status="
                << status_name(state.status)
                << " last_sequence=" << state.last_sequence << '\n';
        return ExitCode::Success;
    }

    if (args[1] == "evaluate-log") {
        if (args.size() != 4 || args[2] != "--events" || args[3].empty()) {
            error_ << "evaluate-log requires --events <event-log path>\n";
            return ExitCode::InvalidInputOrConfig;
        }
        const auto evaluated =
            evaluate_(std::filesystem::u8path(args[3]));
        if (!evaluated.has_value()) {
            error_ << "event log evaluation failed\n";
            return ExitCode::InvalidEventLog;
        }
        const auto& result = evaluated.value();
        if (!valid_generated_task_id(result.task_id)) {
            error_ << "event log contains an invalid task ID\n";
            return ExitCode::InvalidEventLog;
        }
        output_ << "task_id=" << result.task_id
                << " verdict=" << (result.passed ? "pass" : "fail")
                << " status=" << status_name(result.status)
                << " model_rounds=" << result.model_rounds
                << " tool_calls=" << result.tool_calls
                << " evidence_rounds=" << result.evidence_rounds
                << " evidence_items=" << result.evidence_items
                << " model_requests_with_evidence="
                << result.model_requests_with_evidence
                << " tool_error_results=" << result.tool_error_results
                << " last_sequence=" << result.last_sequence << '\n';
        return result.passed ? ExitCode::Success : ExitCode::TaskFailed;
    }

    bool progress_valid = true;
    const RuntimeProgressObserver observer =
        [&](const RuntimeProgress& progress) {
            if (!progress_valid) {
                return;
            }
            if (!valid_generated_task_id(progress.task_id)) {
                progress_valid = false;
                return;
            }
            output_ << "task_id=" << progress.task_id
                    << " sequence=" << progress.sequence
                    << " event=" << event_kind_name(progress.event_kind)
                    << " status=" << status_name(progress.status) << '\n';
        };

    if (args[1] == "resume") {
        if (args.size() != 4 || args[2] != "--task-id" ||
            !valid_generated_task_id(args[3])) {
            error_ << "resume requires --task-id <valid task ID>\n";
            return ExitCode::InvalidInputOrConfig;
        }
        const auto result = resume_(args[3], observer);
        return render_runtime_result(result, progress_valid, output_, error_);
    }

    if (args[1] != "run") {
        error_ << "unknown command\n";
        return ExitCode::InvalidInputOrConfig;
    }

    std::optional<std::string> workspace;
    std::optional<std::string> issue;
    for (std::size_t index = 2; index < args.size(); ++index) {
        const auto& option = args[index];
        if ((option != "--workspace" && option != "--issue") ||
            index + 1 >= args.size()) {
            error_ << "run accepts --workspace <path> and --issue <text>\n";
            return ExitCode::InvalidInputOrConfig;
        }
        const auto& value = args[++index];
        auto& destination = option == "--workspace" ? workspace : issue;
        if (destination.has_value() || value.empty()) {
            error_ << "run options must be present once with non-empty values\n";
            return ExitCode::InvalidInputOrConfig;
        }
        destination = value;
    }
    if (!workspace.has_value() || !issue.has_value()) {
        error_ << "run requires --workspace and --issue\n";
        return ExitCode::InvalidInputOrConfig;
    }

    const auto result =
        run_({*issue, *workspace, std::string{}, {}}, observer);
    return render_runtime_result(result, progress_valid, output_, error_);
}

}  // namespace agent

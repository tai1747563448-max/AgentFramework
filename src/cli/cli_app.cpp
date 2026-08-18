#include "cli/cli_app.h"

#include <filesystem>
#include <ostream>
#include <string>
#include <system_error>
#include <utility>

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
    return Result<StartupArguments>::success(std::move(parsed));
}

CliApp::CliApp(RunCommand run,
               VerifyCommand verify,
               std::ostream& output,
               std::ostream& error)
    : run_(std::move(run)),
      verify_(std::move(verify)),
      output_(output),
      error_(error) {}

int CliApp::execute(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        error_ << "usage: agent run --workspace <path> --issue <text> | "
                  "agent verify-log <path>\n";
        return ExitCode::InvalidInputOrConfig;
    }

    if (args[1] == "verify-log") {
        if (args.size() != 3 || args[2].empty()) {
            error_ << "verify-log requires exactly one event-log path\n";
            return ExitCode::InvalidInputOrConfig;
        }
        const auto verified = verify_(std::filesystem::u8path(args[2]));
        if (!verified.has_value()) {
            error_ << "event log validation failed\n";
            return ExitCode::InvalidEventLog;
        }
        const auto& state = verified.value();
        output_ << "task_id=" << state.task_id << " status="
                << status_name(state.status)
                << " last_sequence=" << state.last_sequence << '\n';
        return ExitCode::Success;
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

    const auto result = run_({*issue, *workspace, std::string{}, {}});
    if (result.fatal_error.has_value()) {
        error_ << "runtime failed before reaching a terminal task state\n";
        return exit_for_error(result.fatal_error->code);
    }
    if (!result.state.has_value()) {
        error_ << "runtime returned no task state\n";
        return ExitCode::TaskFailed;
    }

    switch (result.state->status) {
        case TaskStatus::Completed:
            if (!result.state->final_text.has_value()) {
                error_ << "completed task has no final output\n";
                return ExitCode::TaskFailed;
            }
            output_ << *result.state->final_text << '\n';
            return ExitCode::Success;
        case TaskStatus::BudgetExceeded:
            error_ << "task budget exceeded\n";
            return ExitCode::BudgetExceeded;
        case TaskStatus::Cancelled:
            error_ << "task cancelled\n";
            return ExitCode::Cancelled;
        case TaskStatus::Failed:
            error_ << "task failed\n";
            return ExitCode::TaskFailed;
        default:
            error_ << "runtime returned a non-terminal task state\n";
            return ExitCode::TaskFailed;
    }
}

}  // namespace agent

#pragma once

#include "application/runtime_engine.h"
#include "application/task_evaluator.h"
#include "domain/result.h"
#include "domain/task_state.h"

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace agent {

using RunCommand = std::function<RuntimeResult(
    const RunRequest&, const RuntimeProgressObserver&)>;
using ResumeCommand = std::function<RuntimeResult(
    const std::string&, const RuntimeProgressObserver&)>;
using VerifyCommand =
    std::function<Result<TaskState>(const std::filesystem::path&)>;
using EvaluateCommand =
    std::function<Result<TaskEvaluation>(const std::filesystem::path&)>;

enum ExitCode : int {
    Success = 0,
    InvalidInputOrConfig = 2,
    TaskFailed = 3,
    BudgetExceeded = 4,
    Cancelled = 5,
    PersistenceFailure = 6,
    InvalidEventLog = 7
};

struct StartupArguments {
    std::vector<std::string> command_args;
    std::optional<std::filesystem::path> env_file;
    bool plain_ui{false};
    bool stream_enabled{true};
};

Result<StartupArguments> parse_startup_arguments(
    const std::vector<std::string>& args);

#if defined(_WIN32)
Result<std::vector<std::string>> utf8_arguments_from_windows(
    int argc,
    const wchar_t* const argv[]);
#endif

class CliApp {
public:
    CliApp(RunCommand run,
           VerifyCommand verify,
           std::ostream& output,
           std::ostream& error);
    CliApp(RunCommand run,
           ResumeCommand resume,
           VerifyCommand verify,
           std::ostream& output,
           std::ostream& error);
    CliApp(RunCommand run,
           ResumeCommand resume,
           VerifyCommand verify,
           EvaluateCommand evaluate,
           std::ostream& output,
           std::ostream& error);

    int execute(const std::vector<std::string>& args);

private:
    RunCommand run_;
    ResumeCommand resume_;
    VerifyCommand verify_;
    EvaluateCommand evaluate_;
    std::ostream& output_;
    std::ostream& error_;
};

}  // namespace agent

#pragma once

#include "application/runtime_engine.h"
#include "domain/result.h"
#include "domain/task_state.h"

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace agent {

using RunCommand = std::function<RuntimeResult(const RunRequest&)>;
using VerifyCommand =
    std::function<Result<TaskState>(const std::filesystem::path&)>;

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
};

Result<StartupArguments> parse_startup_arguments(
    const std::vector<std::string>& args);

class CliApp {
public:
    CliApp(RunCommand run,
           VerifyCommand verify,
           std::ostream& output,
           std::ostream& error);

    int execute(const std::vector<std::string>& args);

private:
    RunCommand run_;
    VerifyCommand verify_;
    std::ostream& output_;
    std::ostream& error_;
};

}  // namespace agent

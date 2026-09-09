#pragma once

#include "application/session_engine.h"
#include "cli/cli_app.h"
#include "domain/memory_state.h"

#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

namespace agent {

struct InteractiveSessionCommands {
    std::function<Result<std::vector<SessionState>>()> list;
    std::function<Result<SessionState>(const std::string&)> create;
    std::function<Result<SessionState>(const std::string&)> load;
    std::function<SessionTurnResult(
        const std::string&,
        const std::string&,
        const RuntimeProgressObserver&, bool)> submit;
    std::function<SessionTurnResult(
        const std::string&,
        const RuntimeProgressObserver&, bool)> recover;
    std::function<Result<std::vector<MemoryEntry>>(const std::string&)> memories;
    std::function<Result<MemoryEntry>(const std::string&, const std::string&)> remember;
    std::function<Result<void>(const std::string&)> forget;
    std::function<Result<void>(const std::string&)> consolidate;
};

class InteractiveCli {
public:
    InteractiveCli(InteractiveSessionCommands commands,
                   std::string model,
                   std::string default_workspace,
                   std::istream& input,
                   std::ostream& output,
                   std::ostream& error,
                   bool memory_enabled = true);

    int run();

private:
    RuntimeProgressObserver progress_observer();
    void show_header(const SessionState& session);
    void show_status(const SessionState& session);
    bool render_turn(const SessionTurnResult& result,
                     SessionState& session);
    void consolidate(const SessionState& session);
    Result<std::vector<MemoryEntry>> eligible_memories(const SessionState& session);

    InteractiveSessionCommands commands_;
    std::string model_;
    std::string default_workspace_;
    std::istream& input_;
    std::ostream& output_;
    std::ostream& error_;
    bool memory_available_;
    bool memory_on_;
};

}  // namespace agent

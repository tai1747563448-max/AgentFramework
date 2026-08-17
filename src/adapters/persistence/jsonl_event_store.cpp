#include "adapters/persistence/jsonl_event_store.h"

#include "adapters/persistence/event_json.h"
#include "application/state_reducer.h"

#include <nlohmann/json.hpp>

#include <fstream>
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

}  // namespace

JsonlEventStore::JsonlEventStore(std::filesystem::path runtime_root)
    : runtime_root_(std::move(runtime_root)) {}

std::filesystem::path JsonlEventStore::event_path(
    const std::string& task_id) const {
    return runtime_root_ / "tasks" / std::filesystem::u8path(task_id) /
           "events.jsonl";
}

Result<void> JsonlEventStore::append(const RuntimeEvent& event) {
    try {
        const auto serialized = event_to_json(event).dump();
        const auto path = event_path(event.task_id);
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return persistence_failure("failed to create event directory");
        }

        std::ofstream output(path, std::ios::binary | std::ios::app);
        if (!output) {
            return persistence_failure("failed to open event log for append");
        }
        output.write(serialized.data(),
                     static_cast<std::streamsize>(serialized.size()));
        output.put('\n');
        if (!output) {
            return persistence_failure("failed to append complete event record");
        }
        output.flush();
        if (!output) {
            return persistence_failure("failed to flush event log");
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

}  // namespace agent

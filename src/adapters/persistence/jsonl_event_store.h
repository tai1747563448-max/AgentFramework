#pragma once

#include "ports/event_store.h"

#include <filesystem>
#include <string>

namespace agent {

class JsonlEventStore final : public EventStore {
public:
    explicit JsonlEventStore(std::filesystem::path runtime_root);

    Result<void> append(const RuntimeEvent& event) override;
    Result<std::vector<RuntimeEvent>> read_file(
        const std::filesystem::path& path) const override;
    Result<std::vector<RuntimeEvent>> read_task(
        const std::string& task_id) const;
    Result<std::filesystem::path> event_path(
        const std::string& task_id) const;

private:
    std::filesystem::path runtime_root_;
};

}  // namespace agent

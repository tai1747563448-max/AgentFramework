#pragma once

#include "domain/result.h"
#include "domain/runtime_event.h"

#include <filesystem>
#include <vector>

namespace agent {

class EventStore {
public:
    virtual ~EventStore() = default;
    virtual Result<void> append(const RuntimeEvent& event) = 0;
    virtual Result<std::vector<RuntimeEvent>> read_file(
        const std::filesystem::path& path) const = 0;
};

}  // namespace agent

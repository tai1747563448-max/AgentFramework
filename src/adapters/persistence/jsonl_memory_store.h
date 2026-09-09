#pragma once

#include "ports/memory_store.h"
#include <filesystem>

namespace agent {
class JsonlMemoryStore final : public MemoryStore {
  public:
    explicit JsonlMemoryStore(std::filesystem::path runtime_root);
    Result<void> append(const MemoryEvent &event) override;
    Result<std::vector<MemoryEvent>> read_all() const override;
    Result<MemoryState> read_state() const override;
    Result<std::filesystem::path> event_path() const;

  private:
    std::filesystem::path runtime_root_;
};
} // namespace agent

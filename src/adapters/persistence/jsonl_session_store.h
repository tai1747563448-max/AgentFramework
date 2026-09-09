#pragma once

#include "ports/session_store.h"

#include <filesystem>
#include <string>

namespace agent {

class JsonlSessionStore final : public SessionStore {
public:
    explicit JsonlSessionStore(std::filesystem::path runtime_root);

    Result<void> append(const SessionEvent& event) override;
    Result<std::vector<SessionEvent>> read_session(
        const std::string& session_id) const override;
    Result<std::vector<SessionState>> list_sessions() const override;
    Result<std::filesystem::path> event_path(
        const std::string& session_id) const;

private:
    std::filesystem::path runtime_root_;
};

}  // namespace agent

#pragma once

#include "domain/result.h"
#include "domain/session_event.h"
#include "domain/session_state.h"

#include <string>
#include <vector>

namespace agent {

class SessionStore {
public:
    virtual ~SessionStore() = default;
    virtual Result<void> append(const SessionEvent& event) = 0;
    virtual Result<std::vector<SessionEvent>> read_session(
        const std::string& session_id) const = 0;
    virtual Result<std::vector<SessionState>> list_sessions() const = 0;
};

}  // namespace agent

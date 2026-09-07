#pragma once

#include "domain/result.h"
#include "domain/session_event.h"
#include "domain/session_state.h"

#include <optional>
#include <vector>

namespace agent {

Result<SessionState> reduce_session_event(
    const std::optional<SessionState>& current,
    const SessionEvent& event);

Result<SessionState> replay_session_events(
    const std::vector<SessionEvent>& events);

}  // namespace agent

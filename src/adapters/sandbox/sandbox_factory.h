#pragma once

#include "ports/sandbox.h"

#include <memory>
#include <string>

namespace agent {

// T22: select the platform-appropriate Sandbox implementation at runtime.
// Order:
//   1. macOS  -> SeatbeltSandbox (sandbox-exec is part of the OS).
//   2. Linux  -> BubblewrapSandbox when /usr/bin/bwrap exists on PATH;
//                NoOpSandbox otherwise so behaviour degrades gracefully.
//   3. Windows-> JobObjectSandbox (no extra binary required).
//
// Callers that want to override the selection (tests, custom deployments)
// can call make_sandbox("bwrap") / make_sandbox("none") / etc. directly.
std::unique_ptr<Sandbox> make_default_sandbox();

// Build a sandbox by name. Returns nullptr when the name is unknown;
// adapters that fail platform checks return a live instance whose apply()
// reports UnsupportedPlatform.
std::unique_ptr<Sandbox> make_sandbox(const std::string& name);

}  // namespace agent

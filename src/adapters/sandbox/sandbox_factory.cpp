#include "adapters/sandbox/sandbox_factory.h"

#include "adapters/sandbox/bubblewrap_sandbox.h"
#include "adapters/sandbox/job_object_sandbox.h"
#include "adapters/sandbox/no_op_sandbox.h"
#include "adapters/sandbox/seatbelt_sandbox.h"

namespace agent {

std::unique_ptr<Sandbox> make_sandbox(const std::string& name) {
    if (name == "bwrap") {
        return std::make_unique<BubblewrapSandbox>();
    }
    if (name == "seatbelt") {
        return std::make_unique<SeatbeltSandbox>();
    }
    if (name == "job_object") {
        return std::make_unique<JobObjectSandbox>();
    }
    if (name == "none") {
        return std::make_unique<NoOpSandbox>();
    }
    return nullptr;
}

std::unique_ptr<Sandbox> make_default_sandbox() {
#if defined(__APPLE__)
    return std::make_unique<SeatbeltSandbox>();
#elif defined(_WIN32)
    return std::make_unique<JobObjectSandbox>();
#else
    if (!locate_bubblewrap_binary().empty()) {
        return std::make_unique<BubblewrapSandbox>();
    }
    return std::make_unique<NoOpSandbox>();
#endif
}

}  // namespace agent

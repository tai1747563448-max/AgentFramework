#pragma once

#include "ports/process_runner.h"

namespace agent {

class DirectProcessRunner final : public ProcessRunner {
public:
    Result<ProcessOutput> run(const ProcessRequest& request) override;
};

}  // namespace agent

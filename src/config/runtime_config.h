#pragma once

#include "adapters/anthropic/anthropic_messages_client.h"
#include "domain/result.h"
#include "domain/task_state.h"

#include <filesystem>
#include <optional>
#include <string>

namespace agent {

class Environment {
public:
    virtual ~Environment() = default;
    virtual std::optional<std::string> get(const std::string& name) const = 0;
};

class ProcessEnvironment final : public Environment {
public:
    std::optional<std::string> get(const std::string& name) const override;
};

struct RuntimeConfig {
    AnthropicConfig anthropic;
    RuntimeBudgets budgets;
    std::filesystem::path runtime_root;
    bool build_tools_enabled{false};
    std::int64_t build_timeout_ms{300'000};
    std::string system_prompt;
};

Result<RuntimeConfig> load_runtime_config(const Environment& environment);
Result<void> load_explicit_env_file(const std::filesystem::path& path);

}  // namespace agent

#pragma once

#include "adapters/anthropic/anthropic_messages_client.h"
#include "adapters/rag/persistent_rag_knowledge_provider.h"
#include "application/memory_policy.h"
#include "application/session_engine.h"
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
    bool rag_enabled{false};
    RagConfig rag;
    SessionContextSettings session_context;
    MemoryPolicyConfig memory_policy{1024, {}};
    std::string system_prompt;
};

Result<RuntimeConfig> load_runtime_config(
    const Environment& environment,
    const std::filesystem::path& executable_path = {});
Result<void> load_explicit_env_file(const std::filesystem::path& path);
Result<std::optional<std::filesystem::path>> discover_interactive_env_file(
    const std::filesystem::path& executable_path,
    const std::filesystem::path& current_directory);
Result<std::optional<std::filesystem::path>> discover_rag_pack_root(
    const std::filesystem::path& executable_path);

}  // namespace agent

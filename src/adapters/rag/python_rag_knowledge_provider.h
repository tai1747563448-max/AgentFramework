#pragma once

#include "ports/knowledge_provider.h"
#include "ports/process_runner.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace agent {

struct PythonRagConfig {
    std::string python_program{"python"};
    std::filesystem::path script_path;
    std::filesystem::path index_path;
    std::size_t top_k{5};
    std::int64_t timeout_seconds{10};
};

class PythonRagKnowledgeProvider final : public KnowledgeProvider {
public:
    PythonRagKnowledgeProvider(ProcessRunner& process, PythonRagConfig config);

    Result<EvidencePack> retrieve(const TaskState& state) override;

private:
    ProcessRunner& process_;
    const PythonRagConfig config_;
};

}  // namespace agent

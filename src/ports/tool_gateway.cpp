#include "ports/tool_gateway.h"

#include <utility>

namespace agent {

namespace {

// Concrete Tool backed by a ToolBlueprint. Holds the blueprint by value so
// the returned Tool owns its execute std::function and Value schema; the
// caller can release the source blueprint immediately after build_tool()
// returns. concurrency_safe / read_only are stored once and exposed as
// constants, removing any chance of accidental late mutation.
class BlueprintTool final : public Tool {
public:
    explicit BlueprintTool(ToolBlueprint blueprint)
        : blueprint_(std::move(blueprint)) {}

    std::string name() const override { return blueprint_.name; }
    std::string description() const override {
        return blueprint_.description;
    }
    Value input_schema() const override { return blueprint_.input_schema; }
    Result<ToolResult> execute(
        const ToolCall& call,
        const ToolExecutionContext& context) override {
        if (!blueprint_.execute) {
            return Result<ToolResult>::failure(
                {ErrorCode::InvalidConfiguration,
                 "tool blueprint is missing an execute function",
                 false});
        }
        return blueprint_.execute(call, context);
    }
    bool isConcurrencySafe() const override {
        return blueprint_.concurrency_safe;
    }
    bool isReadOnly() const override { return blueprint_.read_only; }

private:
    ToolBlueprint blueprint_;
};

}  // namespace

std::unique_ptr<Tool> build_tool(ToolBlueprint blueprint) {
    if (blueprint.name.empty() || blueprint.description.empty() ||
        !blueprint.execute) {
        return nullptr;
    }
    return std::make_unique<BlueprintTool>(std::move(blueprint));
}

}  // namespace agent

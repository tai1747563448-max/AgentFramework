#include "adapters/workspace/workspace_tool_gateway.h"

#include "adapters/json/value_json.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace agent {
namespace {

constexpr std::size_t kMaxResultBytes = 65536;
constexpr std::size_t kMaxArgumentsBytes = 2U * 1024U * 1024U;
constexpr std::size_t kMaxValueDepth = 64;
constexpr std::size_t kMaxValueNodes = 10000;

Value string_schema() {
    return Value::object({{"type", "string"}});
}

Value boolean_schema() {
    return Value::object({{"type", "boolean"}});
}

Value integer_schema(std::int64_t minimum, std::int64_t maximum) {
    return Value::object({{"type", "integer"},
                          {"minimum", minimum},
                          {"maximum", maximum}});
}

Value required(std::initializer_list<const char*> names) {
    Value::Array values;
    values.reserve(names.size());
    for (const auto* name : names) {
        values.emplace_back(name);
    }
    return Value::array(std::move(values));
}

ToolDefinition definition(std::string name,
                          std::string description,
                          Value::Object properties,
                          Value required_fields) {
    return {std::move(name), std::move(description),
            Value::object({
                {"type", "object"},
                {"properties", Value::object(std::move(properties))},
                {"required", std::move(required_fields)},
                {"additionalProperties", false}})};
}

workspace::Fault invalid_arguments(std::string message) {
    return {workspace::FaultCode::InvalidArguments, std::move(message), false};
}

const char* fault_code_name(workspace::FaultCode code) {
    switch (code) {
        case workspace::FaultCode::InvalidArguments:
            return "invalid_arguments";
        case workspace::FaultCode::AccessDenied:
            return "access_denied";
        case workspace::FaultCode::NotFound:
            return "not_found";
        case workspace::FaultCode::Conflict:
            return "conflict";
        case workspace::FaultCode::UnsupportedFile:
            return "unsupported_file";
        case workspace::FaultCode::LimitExceeded:
            return "limit_exceeded";
        case workspace::FaultCode::IoError:
            return "io_error";
    }
    return "io_error";
}

ToolResult fault_result(const std::string& call_id,
                        const workspace::Fault& fault) {
    const nlohmann::json content{
        {"error", {{"code", fault_code_name(fault.code)},
                   {"message", fault.message},
                   {"retryable", fault.retryable}}}};
    return {call_id, content.dump(), true};
}

Result<ToolResult> success_result(const std::string& call_id,
                                  nlohmann::json content) {
    auto serialized = content.dump();
    if (serialized.size() > kMaxResultBytes) {
        return Result<ToolResult>::success(fault_result(
            call_id, {workspace::FaultCode::LimitExceeded,
                      "workspace tool result exceeds the limit", false}));
    }
    return Result<ToolResult>::success(
        {call_id, std::move(serialized), false});
}

bool value_budget(const Value& value,
                  std::size_t depth,
                  std::size_t& nodes) {
    if (depth > kMaxValueDepth || ++nodes > kMaxValueNodes) {
        return false;
    }
    if (value.is_array()) {
        for (const auto& item : value.as_array()) {
            if (!value_budget(item, depth + 1, nodes)) {
                return false;
            }
        }
    } else if (value.is_object()) {
        for (const auto& item : value.as_object()) {
            if (!value_budget(item.second, depth + 1, nodes)) {
                return false;
            }
        }
    }
    return true;
}

bool optional_exact_keys(const Value::Object& object,
                         std::initializer_list<const char*> required_keys,
                         std::initializer_list<const char*> optional_keys) {
    std::set<std::string> allowed;
    for (const auto* key : required_keys) {
        allowed.emplace(key);
        if (object.find(key) == object.end()) {
            return false;
        }
    }
    for (const auto* key : optional_keys) {
        allowed.emplace(key);
    }
    return std::all_of(object.begin(), object.end(), [&](const auto& item) {
        return allowed.find(item.first) != allowed.end();
    });
}

std::optional<std::string> string_value(const Value::Object& object,
                                        const char* key) {
    const auto found = object.find(key);
    if (found == object.end() ||
        !std::holds_alternative<std::string>(found->second.storage())) {
        return std::nullopt;
    }
    return found->second.as_string();
}

std::optional<bool> bool_value(const Value::Object& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end() ||
        !std::holds_alternative<bool>(found->second.storage())) {
        return std::nullopt;
    }
    return found->second.as_bool();
}

std::optional<std::size_t> size_value(const Value::Object& object,
                                      const char* key,
                                      std::size_t minimum,
                                      std::size_t maximum) {
    const auto found = object.find(key);
    if (found == object.end() || !found->second.is_integer()) {
        return std::nullopt;
    }
    const auto raw = found->second.as_integer();
    if (raw < 0 || static_cast<std::uint64_t>(raw) < minimum ||
        static_cast<std::uint64_t>(raw) > maximum) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(raw);
}

nlohmann::json list_json(const std::string& path,
                         const workspace::ListOutput& output) {
    auto entries = nlohmann::json::array();
    for (const auto& entry : output.entries) {
        nlohmann::json encoded{{"path", entry.path},
                               {"type", entry.directory ? "directory" : "file"}};
        if (!entry.directory) {
            encoded["size_bytes"] = entry.size_bytes;
        }
        entries.push_back(std::move(encoded));
    }
    return {{"path", path},
            {"entries", std::move(entries)},
            {"truncated", output.truncated},
            {"truncation_reason", output.truncation_reason},
            {"omitted_entries", output.omitted_entries}};
}

nlohmann::json read_json(const workspace::ReadOutput& output) {
    const auto& page = output.page;
    nlohmann::json next = page.next_start_line.has_value()
                              ? nlohmann::json(*page.next_start_line)
                              : nlohmann::json(nullptr);
    return {{"path", output.path},
            {"sha256", output.sha256},
            {"start_line", page.start_line},
            {"end_line", page.end_line},
            {"total_lines", page.total_lines},
            {"content", page.content},
            {"truncated", page.next_start_line.has_value()},
            {"next_start_line", std::move(next)}};
}

}  // namespace

WorkspaceToolGateway::WorkspaceToolGateway(std::filesystem::path runtime_root)
    : policy_(runtime_root), files_(std::move(runtime_root)) {}

std::vector<ToolDefinition> WorkspaceToolGateway::definitions() const {
    return {
        definition("list_files", "List files under the task workspace.",
                   {{"path", string_schema()},
                    {"recursive", boolean_schema()},
                    {"max_results", integer_schema(1, 200)}},
                   required({"path"})),
        definition("read_file", "Read bounded UTF-8 lines from a workspace file.",
                   {{"path", string_schema()},
                    {"start_line", integer_schema(1, 10'000'000)},
                    {"max_lines", integer_schema(1, 1000)}},
                   required({"path"})),
        definition("search_text", "Search literal text under the task workspace.",
                   {{"path", string_schema()},
                    {"query", string_schema()},
                    {"case_sensitive", boolean_schema()},
                    {"max_results", integer_schema(1, 200)}},
                   required({"path", "query"})),
        definition("replace_text", "Replace exact versioned text in one file.",
                   {{"path", string_schema()},
                    {"old_text", string_schema()},
                    {"new_text", string_schema()},
                    {"expected_occurrences", integer_schema(1, 1000)},
                    {"expected_sha256", string_schema()}},
                   required({"path", "old_text", "new_text",
                             "expected_occurrences", "expected_sha256"})),
        definition("write_file", "Create or overwrite one versioned UTF-8 file.",
                   {{"path", string_schema()},
                    {"content", string_schema()},
                    {"mode", Value::object({
                                 {"type", "string"},
                                 {"enum", Value::array({"create", "overwrite"})}})},
                    {"expected_sha256", string_schema()}},
                   required({"path", "content", "mode"}))};
}

Result<ToolResult> WorkspaceToolGateway::execute(
    const ToolCall& call,
    const ToolExecutionContext& context) {
    try {
        std::size_t nodes = 0;
        if (!call.arguments.is_object() ||
            !value_budget(call.arguments, 1, nodes) ||
            value_to_json(call.arguments).dump().size() > kMaxArgumentsBytes) {
            return Result<ToolResult>::success(fault_result(
                call.id, invalid_arguments("invalid tool arguments")));
        }
        const auto& args = call.arguments.as_object();
        const auto workspace = std::filesystem::u8path(context.workspace_utf8);
        if (call.name == "list_files") {
            if (!optional_exact_keys(args, {"path"},
                                     {"recursive", "max_results"})) {
                return Result<ToolResult>::success(fault_result(
                    call.id, invalid_arguments("invalid list_files arguments")));
            }
            const auto path_text = string_value(args, "path");
            const auto recursive = args.find("recursive") == args.end()
                                       ? std::optional<bool>{false}
                                       : bool_value(args, "recursive");
            const auto max_results = args.find("max_results") == args.end()
                                         ? std::optional<std::size_t>{100}
                                         : size_value(args, "max_results", 1, 200);
            if (!path_text.has_value() || !recursive.has_value() ||
                !max_results.has_value()) {
                return Result<ToolResult>::success(fault_result(
                    call.id, invalid_arguments("invalid list_files arguments")));
            }
            const auto parsed = policy_.parse(*path_text);
            if (std::holds_alternative<workspace::Fault>(parsed)) {
                return Result<ToolResult>::success(
                    fault_result(call.id, std::get<workspace::Fault>(parsed)));
            }
            const auto listed = files_.list(
                workspace, std::get<workspace::RelativePath>(parsed),
                *recursive, *max_results);
            if (std::holds_alternative<workspace::Fault>(listed)) {
                return Result<ToolResult>::success(
                    fault_result(call.id, std::get<workspace::Fault>(listed)));
            }
            return success_result(
                call.id, list_json(*path_text,
                                   std::get<workspace::ListOutput>(listed)));
        }
        if (call.name == "read_file") {
            if (!optional_exact_keys(args, {"path"},
                                     {"start_line", "max_lines"})) {
                return Result<ToolResult>::success(fault_result(
                    call.id, invalid_arguments("invalid read_file arguments")));
            }
            const auto path_text = string_value(args, "path");
            const auto start_line = args.find("start_line") == args.end()
                                        ? std::optional<std::size_t>{1}
                                        : size_value(args, "start_line", 1,
                                                     10'000'000);
            const auto max_lines = args.find("max_lines") == args.end()
                                       ? std::optional<std::size_t>{200}
                                       : size_value(args, "max_lines", 1, 1000);
            if (!path_text.has_value() || !start_line.has_value() ||
                !max_lines.has_value()) {
                return Result<ToolResult>::success(fault_result(
                    call.id, invalid_arguments("invalid read_file arguments")));
            }
            const auto parsed = policy_.parse(*path_text);
            if (std::holds_alternative<workspace::Fault>(parsed)) {
                return Result<ToolResult>::success(
                    fault_result(call.id, std::get<workspace::Fault>(parsed)));
            }
            const auto read = files_.read(
                workspace, std::get<workspace::RelativePath>(parsed),
                *start_line, *max_lines);
            if (std::holds_alternative<workspace::Fault>(read)) {
                return Result<ToolResult>::success(
                    fault_result(call.id, std::get<workspace::Fault>(read)));
            }
            return success_result(
                call.id, read_json(std::get<workspace::ReadOutput>(read)));
        }
        return Result<ToolResult>::success(fault_result(
            call.id, invalid_arguments("unknown workspace tool")));
    } catch (...) {
        return Result<ToolResult>::failure(
            {ErrorCode::PersistenceFailure,
             "workspace tool could not produce a trustworthy result", false});
    }
}

}  // namespace agent

#include "adapters/workspace/workspace_tool_gateway.h"

#include "adapters/json/value_json.h"
#include "adapters/workspace/workspace_text.h"
#include "ports/tool_gateway.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
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

bool add_encoded_bytes(std::size_t& encoded, std::size_t bytes) {
    if (bytes > kMaxArgumentsBytes - encoded) {
        return false;
        }
    encoded += bytes;
    return true;
}

bool add_json_string(std::string_view text, std::size_t& encoded) {
    if (!workspace::is_strict_utf8_text(text) ||
        !add_encoded_bytes(encoded, 2)) {
        return false;
    }
    for (const auto raw : text) {
        const auto value = static_cast<unsigned char>(raw);
        std::size_t bytes = 1;
        if (value == static_cast<unsigned char>('"') ||
            value == static_cast<unsigned char>('\\') || value == '\b' ||
            value == '\f' || value == '\n' || value == '\r' || value == '\t') {
            bytes = 2;
        } else if (value < 0x20U) {
            bytes = 6;
        }
        if (!add_encoded_bytes(encoded, bytes)) {
            return false;
        }
    }
    return true;
}

bool bounded_json_value(const Value& value,
                        std::size_t depth,
                        std::size_t& nodes,
                        std::size_t& encoded) {
    if (depth > kMaxValueDepth || ++nodes > kMaxValueNodes) {
        return false;
    }
    if (value.is_array()) {
        if (!add_encoded_bytes(encoded, 2)) {
            return false;
        }
        bool first = true;
        for (const auto& item : value.as_array()) {
            if ((!first && !add_encoded_bytes(encoded, 1)) ||
                !bounded_json_value(item, depth + 1, nodes, encoded)) {
                return false;
            }
            first = false;
        }
    } else if (value.is_object()) {
        if (!add_encoded_bytes(encoded, 2)) {
            return false;
        }
        bool first = true;
        for (const auto& item : value.as_object()) {
            if ((!first && !add_encoded_bytes(encoded, 1)) ||
                !add_json_string(item.first, encoded) ||
                !add_encoded_bytes(encoded, 1) ||
                !bounded_json_value(item.second, depth + 1, nodes, encoded)) {
                return false;
            }
            first = false;
        }
    } else if (std::holds_alternative<std::string>(value.storage())) {
        return add_json_string(value.as_string(), encoded);
    } else if (std::holds_alternative<std::nullptr_t>(value.storage())) {
        return add_encoded_bytes(encoded, 4);
    } else if (std::holds_alternative<bool>(value.storage())) {
        return add_encoded_bytes(encoded, value.as_bool() ? 4 : 5);
    } else if (value.is_integer()) {
        return add_encoded_bytes(encoded,
                                 std::to_string(value.as_integer()).size());
    } else if (value.is_double()) {
        if (!std::isfinite(value.as_double())) {
            return false;
        }
        return add_encoded_bytes(encoded, value_to_json(value).dump().size());
    } else {
        return false;
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

nlohmann::json bounded_list_json(const std::string& path,
                                 workspace::ListOutput output) {
    auto encoded = list_json(path, output);
    while (encoded.dump().size() > kMaxResultBytes &&
           !output.entries.empty()) {
        output.entries.pop_back();
        if (!output.truncated) {
            output.truncated = true;
            output.truncation_reason = "output_bytes";
        }
        encoded = list_json(path, output);
    }
    return encoded;
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

std::optional<nlohmann::json> bounded_read_json(
    workspace::ReadOutput output) {
    auto encoded = read_json(output);
    while (encoded.dump().size() > kMaxResultBytes) {
        auto& page = output.page;
        if (page.content.empty() || page.end_line < page.start_line) {
            return std::nullopt;
        }
        std::size_t before_last_line = page.content.size();
        if (page.content.back() == '\n') {
            --before_last_line;
        }
        const auto separator = before_last_line == 0
                                   ? std::string::npos
                                   : page.content.rfind(
                                         '\n', before_last_line - 1);
        const auto last_line_start = separator == std::string::npos
                                         ? 0
                                         : separator + 1;
        if (last_line_start == 0) {
            return std::nullopt;
        }
        page.content.resize(last_line_start);
        page.next_start_line = page.end_line;
        --page.end_line;
        encoded = read_json(output);
    }
    return encoded;
}

nlohmann::json search_json(const std::string& path,
                           const workspace::SearchOutput& output) {
    auto matches = nlohmann::json::array();
    for (const auto& match : output.matches) {
        matches.push_back({{"path", match.path},
                           {"line", match.line},
                           {"column", match.column},
                           {"text", match.text},
                           {"line_truncated", match.line_truncated}});
    }
    return {{"path", path},
            {"matches", std::move(matches)},
            {"truncated", output.truncated},
            {"truncation_reason", output.truncation_reason},
            {"scanned_files", output.scanned_files},
            {"omitted_entries", output.omitted_entries}};
}

nlohmann::json bounded_search_json(const std::string& path,
                                   workspace::SearchOutput output) {
    auto encoded = search_json(path, output);
    while (encoded.dump().size() > kMaxResultBytes &&
           !output.matches.empty()) {
        output.matches.pop_back();
        if (!output.truncated) {
            output.truncated = true;
            output.truncation_reason = "output_bytes";
        }
        encoded = search_json(path, output);
    }
    return encoded;
}

nlohmann::json write_json(const workspace::WriteOutput& output) {
    return {{"path", output.path},
            {"created", output.created},
            {"old_sha256", output.old_sha256},
            {"new_sha256", output.new_sha256},
            {"replacements", output.replacements},
            {"bytes_written", output.bytes_written}};
}

// Per-tool handlers: each takes the same (call, ctx, files, policy)
// signature so they can be bound into a ToolBlueprint via a thin lambda.
// The handlers themselves remain identical to the previous in-gateway
// branches; only the dispatch wrapper changed.

Result<ToolResult> handle_list_files(const ToolCall& call,
                                     const ToolExecutionContext& context,
                                     const workspace::WorkspaceFileOps& files,
                                     const workspace::WorkspacePathPolicy& policy) {
    const auto& args = call.arguments.as_object();
    const auto workspace = std::filesystem::u8path(context.workspace_utf8);
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
    const auto parsed = policy.parse(*path_text);
    if (std::holds_alternative<workspace::Fault>(parsed)) {
        return Result<ToolResult>::success(
            fault_result(call.id, std::get<workspace::Fault>(parsed)));
    }
    const auto listed = files.list(
        workspace, std::get<workspace::RelativePath>(parsed),
        *recursive, *max_results);
    if (std::holds_alternative<workspace::Fault>(listed)) {
        return Result<ToolResult>::success(
            fault_result(call.id, std::get<workspace::Fault>(listed)));
    }
    return success_result(
        call.id,
        bounded_list_json(
            *path_text, std::get<workspace::ListOutput>(listed)));
}

Result<ToolResult> handle_read_file(const ToolCall& call,
                                    const ToolExecutionContext& context,
                                    const workspace::WorkspaceFileOps& files,
                                    const workspace::WorkspacePathPolicy& policy) {
    const auto& args = call.arguments.as_object();
    const auto workspace = std::filesystem::u8path(context.workspace_utf8);
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
    const auto parsed = policy.parse(*path_text);
    if (std::holds_alternative<workspace::Fault>(parsed)) {
        return Result<ToolResult>::success(
            fault_result(call.id, std::get<workspace::Fault>(parsed)));
    }
    const auto read = files.read(
        workspace, std::get<workspace::RelativePath>(parsed),
        *start_line, *max_lines);
    if (std::holds_alternative<workspace::Fault>(read)) {
        return Result<ToolResult>::success(
            fault_result(call.id, std::get<workspace::Fault>(read)));
    }
    auto read_content = bounded_read_json(
        std::get<workspace::ReadOutput>(read));
    if (!read_content.has_value()) {
        return Result<ToolResult>::success(fault_result(
            call.id,
            {workspace::FaultCode::LimitExceeded,
             "workspace tool result exceeds the limit", false}));
    }
    return success_result(call.id, std::move(*read_content));
}

Result<ToolResult> handle_search_text(const ToolCall& call,
                                      const ToolExecutionContext& context,
                                      const workspace::WorkspaceFileOps& files,
                                      const workspace::WorkspacePathPolicy& policy) {
    const auto& args = call.arguments.as_object();
    const auto workspace = std::filesystem::u8path(context.workspace_utf8);
    if (!optional_exact_keys(args, {"path", "query"},
                             {"case_sensitive", "max_results"})) {
        return Result<ToolResult>::success(fault_result(
            call.id,
            invalid_arguments("invalid search_text arguments")));
    }
    const auto path_text = string_value(args, "path");
    const auto query = string_value(args, "query");
    const auto case_sensitive =
        args.find("case_sensitive") == args.end()
            ? std::optional<bool>{true}
            : bool_value(args, "case_sensitive");
    const auto max_results =
        args.find("max_results") == args.end()
            ? std::optional<std::size_t>{100}
            : size_value(args, "max_results", 1, 200);
    if (!path_text.has_value() || !query.has_value() ||
        !case_sensitive.has_value() || !max_results.has_value()) {
        return Result<ToolResult>::success(fault_result(
            call.id,
            invalid_arguments("invalid search_text arguments")));
    }
    const auto parsed = policy.parse(*path_text);
    if (std::holds_alternative<workspace::Fault>(parsed)) {
        return Result<ToolResult>::success(
            fault_result(call.id, std::get<workspace::Fault>(parsed)));
    }
    const auto searched = files.search(
        workspace, std::get<workspace::RelativePath>(parsed), *query,
        *case_sensitive, *max_results);
    if (std::holds_alternative<workspace::Fault>(searched)) {
        return Result<ToolResult>::success(
            fault_result(call.id, std::get<workspace::Fault>(searched)));
    }
    return success_result(
        call.id,
        bounded_search_json(
            *path_text,
            std::get<workspace::SearchOutput>(searched)));
}

Result<ToolResult> handle_replace_text(const ToolCall& call,
                                       const ToolExecutionContext& context,
                                       const workspace::WorkspaceFileOps& files,
                                       const workspace::WorkspacePathPolicy& policy) {
    const auto& args = call.arguments.as_object();
    const auto workspace = std::filesystem::u8path(context.workspace_utf8);
    if (!optional_exact_keys(
            args,
            {"path", "old_text", "new_text",
             "expected_occurrences", "expected_sha256"},
            {})) {
        return Result<ToolResult>::success(fault_result(
            call.id,
            invalid_arguments("invalid replace_text arguments")));
    }
    const auto path_text = string_value(args, "path");
    const auto old_text = string_value(args, "old_text");
    const auto new_text = string_value(args, "new_text");
    const auto occurrences = size_value(
        args, "expected_occurrences", 1, 1000);
    const auto expected_sha256 =
        string_value(args, "expected_sha256");
    if (!path_text.has_value() || !old_text.has_value() ||
        !new_text.has_value() || !occurrences.has_value() ||
        !expected_sha256.has_value()) {
        return Result<ToolResult>::success(fault_result(
            call.id,
            invalid_arguments("invalid replace_text arguments")));
    }
    const auto parsed = policy.parse(*path_text);
    if (std::holds_alternative<workspace::Fault>(parsed)) {
        return Result<ToolResult>::success(
            fault_result(call.id, std::get<workspace::Fault>(parsed)));
    }
    const auto replaced = files.replace_text(
        workspace, std::get<workspace::RelativePath>(parsed),
        *old_text, *new_text, *occurrences, *expected_sha256);
    if (std::holds_alternative<workspace::Fault>(replaced)) {
        return Result<ToolResult>::success(
            fault_result(call.id, std::get<workspace::Fault>(replaced)));
    }
    return success_result(
        call.id,
        write_json(std::get<workspace::WriteOutput>(replaced)));
}

Result<ToolResult> handle_write_file(const ToolCall& call,
                                     const ToolExecutionContext& context,
                                     const workspace::WorkspaceFileOps& files,
                                     const workspace::WorkspacePathPolicy& policy) {
    const auto& args = call.arguments.as_object();
    const auto workspace = std::filesystem::u8path(context.workspace_utf8);
    if (!optional_exact_keys(
            args, {"path", "content", "mode"},
            {"expected_sha256"})) {
        return Result<ToolResult>::success(fault_result(
            call.id,
            invalid_arguments("invalid write_file arguments")));
    }
    const auto path_text = string_value(args, "path");
    const auto content = string_value(args, "content");
    const auto mode = string_value(args, "mode");
    std::optional<std::string> expected_sha256;
    if (args.find("expected_sha256") != args.end()) {
        expected_sha256 = string_value(args, "expected_sha256");
        if (!expected_sha256.has_value()) {
            return Result<ToolResult>::success(fault_result(
                call.id,
                invalid_arguments("invalid write_file arguments")));
        }
    }
    if (!path_text.has_value() || !content.has_value() ||
        !mode.has_value() ||
        (*mode != "create" && *mode != "overwrite")) {
        return Result<ToolResult>::success(fault_result(
            call.id,
            invalid_arguments("invalid write_file arguments")));
    }
    const auto parsed = policy.parse(*path_text);
    if (std::holds_alternative<workspace::Fault>(parsed)) {
        return Result<ToolResult>::success(
            fault_result(call.id, std::get<workspace::Fault>(parsed)));
    }
    const auto hash_view = expected_sha256.has_value()
                               ? std::optional<std::string_view>{
                                     *expected_sha256}
                               : std::nullopt;
    const auto written = files.write_file(
        workspace, std::get<workspace::RelativePath>(parsed),
        *content, *mode == "create", hash_view);
    if (std::holds_alternative<workspace::Fault>(written)) {
        return Result<ToolResult>::success(
            fault_result(call.id, std::get<workspace::Fault>(written)));
    }
    return success_result(
        call.id,
        write_json(std::get<workspace::WriteOutput>(written)));
}

// Bind a tool name to its static handler with the workspace gateway's
// stable members (files_ / policy_) captured by raw pointer. The
// WorkspaceToolGateway owns the resulting Tool instances, so the captured
// pointers remain valid for the lifetime of the gateway.
template <typename Handler>
std::unique_ptr<Tool> build_workspace_tool(
    std::string name,
    std::string description,
    Value::Object properties,
    Value required_fields,
    bool concurrency_safe,
    bool read_only,
    Handler handler,
    const workspace::WorkspaceFileOps* files,
    const workspace::WorkspacePathPolicy* policy) {
    return build_tool({
        std::move(name),
        std::move(description),
        definition(name, description, std::move(properties),
                   std::move(required_fields)).input_schema,
        [handler, files, policy](const ToolCall& call,
                                const ToolExecutionContext& context) {
            return handler(call, context, *files, *policy);
        },
        concurrency_safe,
        read_only});
}

}  // namespace

WorkspaceToolGateway::WorkspaceToolGateway(std::filesystem::path runtime_root)
    : policy_(runtime_root), files_(std::move(runtime_root)) {
    // T04 (v2 §1): build_tool wires each workspace tool via a fail-closed
    // default. The three read-only tools opt in to concurrency safety;
    // replace_text / write_file stay sequential because their
    // expected_sha256 guards would race under concurrent invocation.
    tools_["list_files"] = build_workspace_tool(
        "list_files", "List files under the task workspace.",
        {{"path", string_schema()},
         {"recursive", boolean_schema()},
         {"max_results", integer_schema(1, 200)}},
        required({"path"}),
        /*concurrency_safe=*/true,
        /*read_only=*/true,
        &handle_list_files, &files_, &policy_);
    tools_["read_file"] = build_workspace_tool(
        "read_file",
        "Read bounded UTF-8 lines from a workspace file.",
        {{"path", string_schema()},
         {"start_line", integer_schema(1, 10'000'000)},
         {"max_lines", integer_schema(1, 1000)}},
        required({"path"}),
        /*concurrency_safe=*/true,
        /*read_only=*/true,
        &handle_read_file, &files_, &policy_);
    tools_["search_text"] = build_workspace_tool(
        "search_text", "Search literal text under the task workspace.",
        {{"path", string_schema()},
         {"query", string_schema()},
         {"case_sensitive", boolean_schema()},
         {"max_results", integer_schema(1, 200)}},
        required({"path", "query"}),
        /*concurrency_safe=*/true,
        /*read_only=*/true,
        &handle_search_text, &files_, &policy_);
    tools_["replace_text"] = build_workspace_tool(
        "replace_text", "Replace exact versioned text in one file.",
        {{"path", string_schema()},
         {"old_text", string_schema()},
         {"new_text", string_schema()},
         {"expected_occurrences", integer_schema(1, 1000)},
         {"expected_sha256", string_schema()}},
        required({"path", "old_text", "new_text",
                  "expected_occurrences", "expected_sha256"}),
        /*concurrency_safe=*/false,
        /*read_only=*/false,
        &handle_replace_text, &files_, &policy_);
    tools_["write_file"] = build_workspace_tool(
        "write_file",
        "Create or overwrite one versioned UTF-8 file.",
        {{"path", string_schema()},
         {"content", string_schema()},
         {"mode", Value::object({
                      {"type", "string"},
                      {"enum", Value::array({"create", "overwrite"})}})},
         {"expected_sha256", string_schema()}},
        required({"path", "content", "mode"}),
        /*concurrency_safe=*/false,
        /*read_only=*/false,
        &handle_write_file, &files_, &policy_);
}

std::vector<ToolDefinition> WorkspaceToolGateway::definitions() const {
    std::vector<ToolDefinition> result;
    result.reserve(tools_.size());
    for (const auto& [name, tool] : tools_) {
        result.push_back({tool->name(), tool->description(),
                          tool->input_schema()});
    }
    return result;
}

Result<ToolResult> WorkspaceToolGateway::execute(
    const ToolCall& call,
    const ToolExecutionContext& context) {
    try {
        std::size_t nodes = 0;
        std::size_t encoded = 0;
        if (!call.arguments.is_object() ||
            !bounded_json_value(call.arguments, 1, nodes, encoded)) {
            return Result<ToolResult>::success(fault_result(
                call.id, invalid_arguments("invalid tool arguments")));
        }
        const auto found = tools_.find(call.name);
        if (found == tools_.end()) {
            return Result<ToolResult>::success(fault_result(
                call.id, invalid_arguments("unknown workspace tool")));
        }
        return found->second->execute(call, context);
    } catch (...) {
        return Result<ToolResult>::failure(
            {ErrorCode::PersistenceFailure,
             "workspace tool could not produce a trustworthy result", false});
    }
}

bool WorkspaceToolGateway::tool_is_concurrency_safe(
    const std::string& name) const {
    const auto found = tools_.find(name);
    if (found == tools_.end()) {
        return false;
    }
    return found->second->isConcurrencySafe();
}

bool WorkspaceToolGateway::tool_is_read_only(
    const std::string& name) const {
    const auto found = tools_.find(name);
    if (found == tools_.end()) {
        return false;
    }
    return found->second->isReadOnly();
}

}  // namespace agent

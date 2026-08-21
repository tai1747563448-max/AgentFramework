#include "adapters/build/cmake_tool_gateway.h"

#include "adapters/workspace/workspace_text.h"

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace agent {
namespace {

constexpr std::size_t kMaxResultBytes = 65'536;
constexpr std::size_t kProcessStreamBytes = 64U * 1024U;
constexpr std::size_t kRetainedPrefixCodePoints = 256;

Value string_schema() {
    return Value::object({{"type", "string"}});
}

Value configuration_schema() {
    return Value::object(
        {{"type", "string"},
         {"enum", Value::array({"Debug", "Release"})}});
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

ToolResult error_result(const std::string& call_id,
                        const char* code,
                        const char* message) {
    const nlohmann::json content{
        {"error", {{"code", code},
                   {"message", message},
                   {"retryable", false}}}};
    return {call_id, content.dump(), true};
}

Result<ToolResult> invalid_arguments(const std::string& call_id) {
    return Result<ToolResult>::success(error_result(
        call_id, "invalid_arguments", "invalid build tool arguments"));
}

Result<ToolResult> access_denied(const std::string& call_id) {
    return Result<ToolResult>::success(error_result(
        call_id, "access_denied", "build workspace is not permitted"));
}

Result<ToolResult> process_unavailable(const std::string& call_id) {
    return Result<ToolResult>::success(error_result(
        call_id, "process_unavailable", "build process is unavailable"));
}

bool exact_keys(const Value::Object& object,
                std::initializer_list<const char*> required_keys,
                std::initializer_list<const char*> optional_keys) {
    std::vector<std::string> allowed;
    allowed.reserve(required_keys.size() + optional_keys.size());
    for (const auto* key : required_keys) {
        allowed.emplace_back(key);
        if (object.find(key) == object.end()) {
            return false;
        }
    }
    for (const auto* key : optional_keys) {
        allowed.emplace_back(key);
    }
    return std::all_of(object.begin(), object.end(), [&](const auto& entry) {
        return std::find(allowed.begin(), allowed.end(), entry.first) !=
               allowed.end();
    });
}

const std::string* string_field(const Value::Object& object,
                                const char* name) {
    const auto found = object.find(name);
    if (found == object.end() ||
        !std::holds_alternative<std::string>(found->second.storage())) {
        return nullptr;
    }
    return &found->second.as_string();
}

bool valid_configuration(const std::string& configuration) {
    return configuration == "Debug" || configuration == "Release";
}

bool valid_target(const std::string& target) {
    if (target.empty() || target.size() > 128) {
        return false;
    }
    return std::all_of(target.begin(), target.end(), [](unsigned char value) {
        return (value >= static_cast<unsigned char>('a') &&
                value <= static_cast<unsigned char>('z')) ||
               (value >= static_cast<unsigned char>('A') &&
                value <= static_cast<unsigned char>('Z')) ||
               (value >= static_cast<unsigned char>('0') &&
                value <= static_cast<unsigned char>('9')) ||
               value == static_cast<unsigned char>('_') ||
               value == static_cast<unsigned char>('.') ||
               value == static_cast<unsigned char>('+') ||
               value == static_cast<unsigned char>('-');
    });
}

bool path_equal(const std::filesystem::path& left,
                const std::filesystem::path& right) {
#if defined(_WIN32)
    const auto left_text = left.lexically_normal().native();
    const auto right_text = right.lexically_normal().native();
    return CompareStringOrdinal(left_text.c_str(), -1, right_text.c_str(), -1,
                                TRUE) == CSTR_EQUAL;
#else
    return left.lexically_normal() == right.lexically_normal();
#endif
}

bool contained(const std::filesystem::path& root,
               const std::filesystem::path& candidate) {
    const auto relative = candidate.lexically_relative(root);
    if (relative.empty() || relative.is_absolute()) {
        return false;
    }
    const auto first = relative.begin();
    return first != relative.end() && *first != std::filesystem::path("..");
}

bool is_reparse_or_link(const std::filesystem::path& path,
                        std::error_code& error) {
    const auto status = std::filesystem::symlink_status(path, error);
    if (error) {
        return false;
    }
    if (std::filesystem::is_symlink(status)) {
        return true;
    }
#if defined(_WIN32)
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = std::error_code(static_cast<int>(GetLastError()),
                                std::system_category());
        return false;
    }
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return false;
#endif
}

bool existing_real_directory(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error ||
        !std::filesystem::is_directory(path, error) || error ||
        is_reparse_or_link(path, error) || error) {
        return false;
    }
    return true;
}

bool create_or_validate_directory(const std::filesystem::path& root,
                                  const std::filesystem::path& path) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        return false;
    }
    if (!exists) {
        if (!std::filesystem::create_directory(path, error) || error) {
            return false;
        }
    }
    if (!existing_real_directory(path)) {
        return false;
    }
    const auto physical = std::filesystem::weakly_canonical(path, error);
    return !error && path_equal(path, physical) && contained(root, physical);
}

bool prepare_build_directory(const std::string& workspace_utf8,
                             std::filesystem::path& root,
                             std::filesystem::path& build) {
    if (workspace_utf8.empty() ||
        !workspace::is_strict_utf8_text(workspace_utf8)) {
        return false;
    }
    std::error_code error;
    const auto supplied = std::filesystem::u8path(workspace_utf8);
    const auto absolute =
        std::filesystem::absolute(supplied, error).lexically_normal();
    if (error || !existing_real_directory(absolute)) {
        return false;
    }
    root = std::filesystem::weakly_canonical(absolute, error);
    if (error || !path_equal(absolute, root)) {
        return false;
    }

    const auto control = root / ".agent";
    if (!create_or_validate_directory(root, control)) {
        return false;
    }
    build = control / "cmake-build";
    if (!create_or_validate_directory(root, build)) {
        return false;
    }
    const auto final_physical = std::filesystem::weakly_canonical(build, error);
    return !error && path_equal(build, final_physical) &&
           contained(root, final_physical);
}

std::string exact_test_regex(const std::string& name) {
    constexpr const char* metacharacters = R"(\.^$|()[]*+?{})";
    std::string escaped;
    escaped.reserve(name.size() + 2);
    escaped.push_back('^');
    for (const char value : name) {
        if (std::string_view(metacharacters).find(value) !=
            std::string_view::npos) {
            escaped.push_back('\\');
        }
        escaped.push_back(value);
    }
    escaped.push_back('$');
    return escaped;
}

std::vector<std::size_t> utf8_boundaries(const std::string& text) {
    std::vector<std::size_t> boundaries;
    boundaries.reserve(text.size() + 1);
    for (std::size_t offset = 0; offset < text.size();) {
        boundaries.push_back(offset);
        const auto first = static_cast<unsigned char>(text[offset]);
        if (first <= 0x7FU) {
            ++offset;
        } else if (first <= 0xDFU) {
            offset += 2;
        } else if (first <= 0xEFU) {
            offset += 3;
        } else {
            offset += 4;
        }
    }
    boundaries.push_back(text.size());
    return boundaries;
}

std::size_t encoded_payload_size(const std::string& text) {
    return nlohmann::json(text).dump().size() - 2;
}

std::string retain_prefix_and_tail(
    const std::string& text,
    const std::vector<std::size_t>& boundaries,
    std::size_t code_points) {
    const auto total = boundaries.size() - 1;
    if (code_points >= total) {
        return text;
    }
    const auto prefix = code_points > kRetainedPrefixCodePoints
                            ? kRetainedPrefixCodePoints
                            : 0U;
    const auto tail = code_points - prefix;
    std::string retained = text.substr(0, boundaries[prefix]);
    retained.append(text.substr(boundaries[total - tail]));
    return retained;
}

std::string fit_encoded_payload(const std::string& text,
                                std::size_t budget) {
    if (encoded_payload_size(text) <= budget) {
        return text;
    }
    const auto boundaries = utf8_boundaries(text);
    std::size_t kept = boundaries.size() - 1;
    while (kept > 0) {
        const auto candidate =
            retain_prefix_and_tail(text, boundaries, kept);
        const auto encoded = encoded_payload_size(candidate);
        if (encoded <= budget) {
            return candidate;
        }
        const auto scaled = static_cast<std::size_t>(
            static_cast<long double>(kept) *
            static_cast<long double>(budget) /
            static_cast<long double>(encoded));
        kept = scaled < kept ? scaled : kept - 1;
    }
    return {};
}

nlohmann::json process_json(const char* operation,
                            const ProcessOutput& output,
                            std::string stdout_text,
                            std::string stderr_text,
                            bool stdout_truncated,
                            bool stderr_truncated) {
    return {{"operation", operation},
            {"exit_code", output.exit_code},
            {"timed_out", output.timed_out},
            {"duration_ms", output.duration_ms},
            {"stdout", std::move(stdout_text)},
            {"stderr", std::move(stderr_text)},
            {"stdout_truncated", stdout_truncated},
            {"stderr_truncated", stderr_truncated}};
}

Result<ToolResult> bounded_process_result(const std::string& call_id,
                                          const char* operation,
                                          const ProcessOutput& output) {
    if (!workspace::is_strict_utf8_text(output.stdout_utf8) ||
        !workspace::is_strict_utf8_text(output.stderr_utf8)) {
        return process_unavailable(call_id);
    }
    auto stdout_text = output.stdout_utf8;
    auto stderr_text = output.stderr_utf8;
    bool stdout_truncated = output.stdout_truncated;
    bool stderr_truncated = output.stderr_truncated;

    const auto base = process_json(operation, output, {}, {}, false, false);
    const auto base_size = base.dump().size();
    if (base_size > kMaxResultBytes) {
        return process_unavailable(call_id);
    }
    const auto available = kMaxResultBytes - base_size;
    const auto stdout_size = encoded_payload_size(stdout_text);
    const auto stderr_size = encoded_payload_size(stderr_text);
    if (stdout_size + stderr_size > available) {
        auto stdout_budget = available / 2;
        auto stderr_budget = available - stdout_budget;
        if (stdout_size < stdout_budget) {
            stdout_budget = stdout_size;
            stderr_budget = available - stdout_budget;
        } else if (stderr_size < stderr_budget) {
            stderr_budget = stderr_size;
            stdout_budget = available - stderr_budget;
        }
        if (stdout_size > stdout_budget) {
            stdout_text = fit_encoded_payload(stdout_text, stdout_budget);
            stdout_truncated = true;
        }
        if (stderr_size > stderr_budget) {
            stderr_text = fit_encoded_payload(stderr_text, stderr_budget);
            stderr_truncated = true;
        }
    }

    auto content = process_json(operation, output, std::move(stdout_text),
                                std::move(stderr_text), stdout_truncated,
                                stderr_truncated)
                       .dump();
    if (content.size() > kMaxResultBytes) {
        return process_unavailable(call_id);
    }
    return Result<ToolResult>::success(
        {call_id, std::move(content),
         output.timed_out || output.exit_code != 0});
}

}  // namespace

CMakeToolGateway::CMakeToolGateway(ProcessRunner& process_runner,
                                   std::int64_t timeout_ms)
    : process_runner_(process_runner), timeout_ms_(timeout_ms) {
    if (timeout_ms_ <= 0 || timeout_ms_ > 600'000) {
        throw std::invalid_argument("build timeout is outside the allowed range");
    }
}

std::vector<ToolDefinition> CMakeToolGateway::definitions() const {
    return {
        definition("configure_project",
                   "Configure the trusted workspace with CMake.",
                   {{"configuration", configuration_schema()}},
                   required({"configuration"})),
        definition("build_project", "Build the configured CMake workspace.",
                   {{"configuration", configuration_schema()},
                    {"target", string_schema()}},
                   required({"configuration"})),
        definition("run_tests", "Run CTest in the configured workspace.",
                   {{"configuration", configuration_schema()},
                    {"test_name", string_schema()}},
                   required({"configuration"}))};
}

Result<ToolResult> CMakeToolGateway::execute(
    const ToolCall& call,
    const ToolExecutionContext& context) {
    if (!call.arguments.is_object()) {
        return invalid_arguments(call.id);
    }
    const auto& arguments = call.arguments.as_object();
    const std::string* configuration = nullptr;
    const std::string* optional_value = nullptr;
    const char* operation = nullptr;

    if (call.name == "configure_project") {
        if (!exact_keys(arguments, {"configuration"}, {})) {
            return invalid_arguments(call.id);
        }
        operation = "configure";
    } else if (call.name == "build_project") {
        if (!exact_keys(arguments, {"configuration"}, {"target"})) {
            return invalid_arguments(call.id);
        }
        operation = "build";
        if (arguments.find("target") != arguments.end()) {
            optional_value = string_field(arguments, "target");
            if (optional_value == nullptr || !valid_target(*optional_value)) {
                return invalid_arguments(call.id);
            }
        }
    } else if (call.name == "run_tests") {
        if (!exact_keys(arguments, {"configuration"}, {"test_name"})) {
            return invalid_arguments(call.id);
        }
        operation = "test";
        if (arguments.find("test_name") != arguments.end()) {
            optional_value = string_field(arguments, "test_name");
            if (optional_value == nullptr || optional_value->empty() ||
                optional_value->size() > 200 ||
                !workspace::is_strict_utf8_text(*optional_value)) {
                return invalid_arguments(call.id);
            }
        }
    } else {
        return invalid_arguments(call.id);
    }

    configuration = string_field(arguments, "configuration");
    if (configuration == nullptr || !valid_configuration(*configuration)) {
        return invalid_arguments(call.id);
    }

    std::filesystem::path root;
    std::filesystem::path build;
    try {
        if (!prepare_build_directory(context.workspace_utf8, root, build)) {
            return access_denied(call.id);
        }
    } catch (...) {
        return access_denied(call.id);
    }

    ProcessRequest request;
    request.working_directory = root;
    request.timeout_ms = timeout_ms_;
    request.max_stdout_bytes = kProcessStreamBytes;
    request.max_stderr_bytes = kProcessStreamBytes;
    request.environment_overrides = {{"VSLANG", "1033"},
                                     {"CLICOLOR", "0"}};
    if (call.name == "configure_project") {
        request.program = "cmake";
        request.arguments = {"-S", root.generic_u8string(), "-B",
                             build.generic_u8string(),
                             "-DCMAKE_BUILD_TYPE=" + *configuration};
    } else if (call.name == "build_project") {
        request.program = "cmake";
        request.arguments = {"--build", build.generic_u8string(), "--config",
                             *configuration};
        if (optional_value != nullptr) {
            request.arguments.push_back("--target");
            request.arguments.push_back(*optional_value);
        }
    } else {
        request.program = "ctest";
        request.arguments = {"--test-dir", build.generic_u8string(), "-C",
                             *configuration, "--output-on-failure",
                             "--no-tests=error"};
        if (optional_value != nullptr) {
            request.arguments.push_back("-R");
            request.arguments.push_back(exact_test_regex(*optional_value));
        }
    }

    try {
        auto output = process_runner_.run(request);
        if (!output.has_value()) {
            return process_unavailable(call.id);
        }
        return bounded_process_result(call.id, operation, output.value());
    } catch (...) {
        return process_unavailable(call.id);
    }
}

}  // namespace agent

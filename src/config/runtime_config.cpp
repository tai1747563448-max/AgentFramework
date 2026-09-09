#include "config/runtime_config.h"

#include <dotenv.h>

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <streambuf>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace agent {
namespace {

constexpr const char* kDefaultSystemPrompt =
    "You are a coding agent. Inspect the workspace, make focused edits, "
    "and verify the result. Use only the tools explicitly provided.";

class NullOutputBuffer final : public std::streambuf {
protected:
    int_type overflow(int_type character) override {
        return traits_type::not_eof(character);
    }
};

class ScopedCoutSilencer final {
public:
    ScopedCoutSilencer() : previous_(std::cout.rdbuf(&sink_)) {}
    ~ScopedCoutSilencer() {
        std::cout.rdbuf(previous_);
    }

    ScopedCoutSilencer(const ScopedCoutSilencer&) = delete;
    ScopedCoutSilencer& operator=(const ScopedCoutSilencer&) = delete;

private:
    NullOutputBuffer sink_;
    std::streambuf* previous_;
};

Result<RuntimeConfig> invalid_config(const char* message) {
    return Result<RuntimeConfig>::failure(
        {ErrorCode::InvalidConfiguration, message, false});
}

std::optional<std::string> nonempty(const Environment& environment,
                                    const char* name) {
    auto value = environment.get(name);
    if (!value.has_value() || value->empty()) {
        return std::nullopt;
    }
    return value;
}

Result<std::uint64_t> positive_integer(const Environment& environment,
                                       const char* name,
                                       std::uint64_t default_value) {
    const auto configured = environment.get(name);
    if (!configured.has_value()) {
        return Result<std::uint64_t>::success(default_value);
    }
    std::uint64_t value = 0;
    const char* begin = configured->data();
    const char* end = begin + configured->size();
    const auto parsed = std::from_chars(begin, end, value, 10);
    if (configured->empty() || parsed.ec != std::errc{} ||
        parsed.ptr != end || value == 0) {
        return Result<std::uint64_t>::failure(
            {ErrorCode::InvalidConfiguration,
             "numeric configuration must be a positive in-range integer",
             false});
    }
    return Result<std::uint64_t>::success(value);
}

Result<bool> exact_flag(const Environment& environment,
                        const char* name,
                        bool default_value) {
    const auto configured = environment.get(name);
    if (!configured.has_value()) {
        return Result<bool>::success(default_value);
    }
    if (*configured == "0") {
        return Result<bool>::success(false);
    }
    if (*configured == "1") {
        return Result<bool>::success(true);
    }
    return Result<bool>::failure(
        {ErrorCode::InvalidConfiguration,
         "flag configuration must be exactly 0 or 1", false});
}

Result<std::size_t> bounded_size(const Environment& environment,
                                 const char* name,
                                 std::uint64_t default_value,
                                 std::uint64_t maximum) {
    const auto parsed = positive_integer(environment, name, default_value);
    if (!parsed.has_value() || parsed.value() > maximum ||
        parsed.value() > std::numeric_limits<std::size_t>::max()) {
        return Result<std::size_t>::failure(
            {ErrorCode::InvalidConfiguration,
             "memory and compaction numbers must be positive bounded integers", false});
    }
    return Result<std::size_t>::success(static_cast<std::size_t>(parsed.value()));
}

Result<std::string> dotenv_filename(const std::filesystem::path& path) {
#if defined(_WIN32)
    std::wstring usable_path = path.native();
    const DWORD short_size =
        GetShortPathNameW(path.c_str(), nullptr, 0);
    if (short_size != 0) {
        std::wstring short_path(short_size, L'\0');
        const DWORD written =
            GetShortPathNameW(path.c_str(), short_path.data(), short_size);
        if (written != 0 && written < short_size) {
            short_path.resize(written);
            usable_path = std::move(short_path);
        }
    }

    BOOL used_default_character = FALSE;
    const int byte_count = WideCharToMultiByte(
        CP_ACP, WC_NO_BEST_FIT_CHARS, usable_path.c_str(),
        static_cast<int>(usable_path.size()), nullptr, 0, nullptr,
        &used_default_character);
    if (byte_count <= 0 || used_default_character != FALSE) {
        return Result<std::string>::failure(
            {ErrorCode::InvalidConfiguration,
             "the explicitly supplied environment file path is unsupported",
             false});
    }
    std::string filename(static_cast<std::size_t>(byte_count), '\0');
    used_default_character = FALSE;
    const int converted = WideCharToMultiByte(
        CP_ACP, WC_NO_BEST_FIT_CHARS, usable_path.c_str(),
        static_cast<int>(usable_path.size()), filename.data(), byte_count,
        nullptr, &used_default_character);
    if (converted != byte_count || used_default_character != FALSE) {
        return Result<std::string>::failure(
            {ErrorCode::InvalidConfiguration,
             "the explicitly supplied environment file path is unsupported",
             false});
    }
    return Result<std::string>::success(std::move(filename));
#else
    return Result<std::string>::success(path.generic_u8string());
#endif
}

#if defined(_WIN32)
std::optional<std::wstring> utf8_to_wide(const std::string& utf8) {
    if (utf8.empty() ||
        utf8.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    const auto length = static_cast<int>(utf8.size());
    const int wide_count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), length, nullptr, 0);
    if (wide_count <= 0) {
        return std::nullopt;
    }
    std::wstring wide(static_cast<std::size_t>(wide_count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                            length, wide.data(), wide_count) != wide_count) {
        return std::nullopt;
    }
    return wide;
}

std::optional<std::string> wide_to_utf8(const std::wstring& wide) {
    if (wide.empty()) {
        return std::string{};
    }
    if (wide.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    const auto length = static_cast<int>(wide.size());
    const int byte_count = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), length, nullptr, 0,
        nullptr, nullptr);
    if (byte_count <= 0) {
        return std::nullopt;
    }
    std::string utf8(static_cast<std::size_t>(byte_count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), length,
                            utf8.data(), byte_count, nullptr,
                            nullptr) != byte_count) {
        return std::nullopt;
    }
    return utf8;
}
#endif

}  // namespace

std::optional<std::string> ProcessEnvironment::get(
    const std::string& name) const {
#if defined(_WIN32)
    const auto wide_name = utf8_to_wide(name);
    if (!wide_name.has_value()) {
        return std::nullopt;
    }
    SetLastError(ERROR_SUCCESS);
    DWORD required = GetEnvironmentVariableW(wide_name->c_str(), nullptr, 0);
    if (required == 0) {
        if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
            return std::nullopt;
        }
        return std::string{};
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::wstring wide_value(static_cast<std::size_t>(required), L'\0');
        const DWORD written = GetEnvironmentVariableW(
            wide_name->c_str(), wide_value.data(), required);
        if (written == 0) {
            if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
                return std::nullopt;
            }
            return std::string{};
        }
        if (written < required) {
            wide_value.resize(written);
            return wide_to_utf8(wide_value);
        }
        required = written + 1;
    }
    return std::nullopt;
#else
    const char* value = std::getenv(name.c_str());
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

Result<RuntimeConfig> load_runtime_config(const Environment& environment) {
    const auto base_url = nonempty(environment, "AGENT_BASE_URL");
    if (!base_url.has_value()) {
        return invalid_config("AGENT_BASE_URL is required");
    }
    const auto model = nonempty(environment, "AGENT_MODEL");
    if (!model.has_value()) {
        return invalid_config("AGENT_MODEL is required");
    }

    const auto api_key = nonempty(environment, "AGENT_API_KEY");
    const auto auth_token = nonempty(environment, "AGENT_AUTH_TOKEN");
    if (api_key.has_value() == auth_token.has_value()) {
        return invalid_config("exactly one authentication mode is required");
    }

    const auto max_tokens =
        positive_integer(environment, "AGENT_MAX_TOKENS", 4096);
    const auto model_rounds =
        positive_integer(environment, "AGENT_MAX_MODEL_ROUNDS", 16);
    const auto tool_calls =
        positive_integer(environment, "AGENT_MAX_TOOL_CALLS", 64);
    const auto task_seconds =
        positive_integer(environment, "AGENT_MAX_TASK_SECONDS", 1800);
    const auto timeout_seconds =
        positive_integer(environment, "AGENT_MODEL_TIMEOUT_SECONDS", 120);
    const auto build_tools_enabled =
        exact_flag(environment, "AGENT_ENABLE_BUILD_TOOLS", false);
    const auto build_timeout_seconds =
        positive_integer(environment, "AGENT_BUILD_TIMEOUT_SECONDS", 300);
    const auto rag_enabled =
        exact_flag(environment, "AGENT_ENABLE_RAG", false);
    const auto memory_enabled =
        exact_flag(environment, "AGENT_ENABLE_MEMORY", true);
    // Compaction needs valid caps even with memory disabled.
    constexpr std::uint64_t kMemoryByteLimit = 1024 * 1024;
    constexpr std::uint64_t kContextByteLimit = 16 * 1024 * 1024;
    const auto memory_top_k = bounded_size(environment, "AGENT_MEMORY_TOP_K", 5, 20);
    const auto memory_injected = bounded_size(environment,
        "AGENT_MEMORY_MAX_INJECTED_BYTES", 4096, kMemoryByteLimit);
    const auto memory_entry = bounded_size(environment,
        "AGENT_MEMORY_MAX_ENTRY_BYTES", 1024, kMemoryByteLimit);
    const auto compaction_threshold = bounded_size(environment,
        "AGENT_COMPACTION_THRESHOLD_BYTES", 65536, kContextByteLimit);
    const auto compaction_hard_limit = bounded_size(environment,
        "AGENT_COMPACTION_HARD_LIMIT_BYTES", 131072, kContextByteLimit);
    const auto compaction_retain = bounded_size(environment,
        "AGENT_COMPACTION_RETAIN_TURNS", 6, 10000);
    const auto compaction_summary = bounded_size(environment,
        "AGENT_COMPACTION_MAX_SUMMARY_BYTES", 8192, 8192);
    if (!max_tokens.has_value() || !model_rounds.has_value() ||
        !tool_calls.has_value() || !task_seconds.has_value() ||
        !timeout_seconds.has_value() || !build_timeout_seconds.has_value()) {
        return invalid_config(
            "numeric configuration must be a positive in-range integer");
    }
    if (!build_tools_enabled.has_value()) {
        return invalid_config("build tool flag must be exactly 0 or 1");
    }
    if (!rag_enabled.has_value()) {
        return invalid_config("rag flag must be exactly 0 or 1");
    }
    if (!memory_enabled.has_value()) {
        return invalid_config("memory flag must be exactly 0 or 1");
    }
    if (!memory_top_k.has_value() || !memory_injected.has_value() ||
        !memory_entry.has_value() || !compaction_threshold.has_value() ||
        !compaction_hard_limit.has_value() || !compaction_retain.has_value() ||
        !compaction_summary.has_value()) {
        return invalid_config(
            "memory and compaction numbers must be positive bounded integers");
    }
    if (compaction_hard_limit.value() <= compaction_threshold.value()) {
        return invalid_config("compaction hard limit must exceed threshold");
    }
    if (build_timeout_seconds.value() > 600) {
        return invalid_config(
            "build timeout must be between 1 and 600 seconds");
    }

    constexpr auto kInt64Max =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (max_tokens.value() > kInt64Max ||
        model_rounds.value() > std::numeric_limits<std::size_t>::max() ||
        tool_calls.value() > std::numeric_limits<std::size_t>::max() ||
        task_seconds.value() > kInt64Max / 1000 ||
        timeout_seconds.value() > kInt64Max / 1000 ||
        build_timeout_seconds.value() > kInt64Max / 1000) {
        return invalid_config("numeric configuration is out of range");
    }

    const auto runtime_root_value = environment.get("AGENT_RUNTIME_ROOT");
    if (runtime_root_value.has_value() && runtime_root_value->empty()) {
        return invalid_config("AGENT_RUNTIME_ROOT must not be empty");
    }
    const auto system_prompt = environment.get("AGENT_SYSTEM_PROMPT");

    PythonRagConfig rag;
    if (rag_enabled.value()) {
        const auto python = environment.get("AGENT_RAG_PYTHON");
        if (python.has_value() && python->empty()) {
            return invalid_config("AGENT_RAG_PYTHON must not be empty");
        }
        const auto script = nonempty(environment, "AGENT_RAG_SCRIPT");
        const auto index = nonempty(environment, "AGENT_RAG_INDEX");
        if (!script.has_value() || !index.has_value()) {
            return invalid_config(
                "enabled rag requires script and index paths");
        }
        const auto top_k =
            positive_integer(environment, "AGENT_RAG_TOP_K", 5);
        const auto rag_timeout =
            positive_integer(environment, "AGENT_RAG_TIMEOUT_SECONDS", 10);
        if (!top_k.has_value() || !rag_timeout.has_value()) {
            return invalid_config(
                "rag numeric configuration must be a positive integer");
        }
        if (top_k.value() > 20) {
            return invalid_config("rag top-k must be between 1 and 20");
        }
        if (rag_timeout.value() > 60) {
            return invalid_config("rag timeout must be between 1 and 60 seconds");
        }
        try {
            rag.python_program = python.has_value() ? *python : "python";
            rag.script_path = std::filesystem::u8path(*script);
            rag.index_path = std::filesystem::u8path(*index);
            rag.top_k = static_cast<std::size_t>(top_k.value());
            rag.timeout_seconds =
                static_cast<std::int64_t>(rag_timeout.value());
        } catch (const std::filesystem::filesystem_error&) {
            return invalid_config("rag path configuration is invalid");
        }
    }

    RuntimeConfig config;
    config.anthropic = {*base_url,
                        *model,
                        api_key.has_value() ? CredentialKind::ApiKey
                                            : CredentialKind::Bearer,
                        api_key.has_value() ? *api_key : *auth_token,
                        "2023-06-01",
                        static_cast<std::int64_t>(max_tokens.value())};
    config.budgets = {
        static_cast<std::size_t>(model_rounds.value()),
        static_cast<std::size_t>(tool_calls.value()),
        static_cast<std::int64_t>(task_seconds.value() * 1000),
        static_cast<std::int64_t>(timeout_seconds.value() * 1000)};
    config.runtime_root = runtime_root_value.has_value()
                              ? std::filesystem::u8path(*runtime_root_value)
                              : std::filesystem::path("runtime_data");
    config.build_tools_enabled = build_tools_enabled.value();
    config.build_timeout_ms = static_cast<std::int64_t>(
        build_timeout_seconds.value() * 1000);
    config.rag_enabled = rag_enabled.value();
    config.rag = std::move(rag);
    config.session_context = {true, compaction_threshold.value(),
        compaction_hard_limit.value(), compaction_retain.value(),
        compaction_summary.value(), memory_top_k.value(), memory_injected.value(),
        memory_enabled.value()};
    config.memory_policy = {memory_entry.value(), {config.anthropic.credential}};
    config.system_prompt =
        system_prompt.has_value() ? *system_prompt : kDefaultSystemPrompt;
    return Result<RuntimeConfig>::success(std::move(config));
}

Result<void> load_explicit_env_file(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        return Result<void>::failure(
            {ErrorCode::InvalidConfiguration,
             "the explicitly supplied environment file cannot be opened",
             false});
    }
    auto filename = dotenv_filename(path);
    if (!filename.has_value()) {
        return Result<void>::failure(filename.error());
    }
    ScopedCoutSilencer silence_library_diagnostics;
    dotenv::init(dotenv::Preserve, filename.value().c_str());
    return Result<void>::success();
}

Result<std::optional<std::filesystem::path>> discover_interactive_env_file(
    const std::filesystem::path& executable_path,
    const std::filesystem::path& current_directory) {
    try {
        std::error_code error;
        const auto absolute_executable = std::filesystem::absolute(
            executable_path, error).lexically_normal();
        if (error) {
            return Result<std::optional<std::filesystem::path>>::failure(
                {ErrorCode::InvalidConfiguration,
                 "interactive executable path could not be resolved", false});
        }
        const auto absolute_cwd = std::filesystem::absolute(
            current_directory, error).lexically_normal();
        if (error) {
            return Result<std::optional<std::filesystem::path>>::failure(
                {ErrorCode::InvalidConfiguration,
                 "interactive working directory could not be resolved", false});
        }
        const std::vector<std::filesystem::path> candidates{
            absolute_executable.parent_path() / ".env",
            absolute_cwd / ".env"};
        for (const auto& candidate : candidates) {
            error.clear();
            const bool exists = std::filesystem::exists(candidate, error);
            if (error) {
                return Result<std::optional<std::filesystem::path>>::failure(
                    {ErrorCode::InvalidConfiguration,
                     "interactive environment file could not be inspected",
                     false});
            }
            if (!exists) {
                continue;
            }
            if (std::filesystem::is_regular_file(candidate, error) && !error) {
                return Result<std::optional<std::filesystem::path>>::success(
                    candidate);
            }
            if (error) {
                return Result<std::optional<std::filesystem::path>>::failure(
                    {ErrorCode::InvalidConfiguration,
                     "interactive environment file could not be inspected",
                     false});
            }
            return Result<std::optional<std::filesystem::path>>::failure(
                {ErrorCode::InvalidConfiguration,
                 "interactive environment path is not a regular file", false});
        }
        return Result<std::optional<std::filesystem::path>>::success(
            std::nullopt);
    } catch (const std::filesystem::filesystem_error&) {
        return Result<std::optional<std::filesystem::path>>::failure(
            {ErrorCode::InvalidConfiguration,
             "interactive environment discovery failed", false});
    }
}

}  // namespace agent

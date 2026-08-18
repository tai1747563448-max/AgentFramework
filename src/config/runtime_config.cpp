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

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace agent {
namespace {

constexpr const char* kDefaultSystemPrompt =
    "You are a coding agent runtime. Use only tools explicitly provided.";

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

}  // namespace

std::optional<std::string> ProcessEnvironment::get(
    const std::string& name) const {
    const char* value = std::getenv(name.c_str());
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
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
    if (!max_tokens.has_value() || !model_rounds.has_value() ||
        !tool_calls.has_value() || !task_seconds.has_value() ||
        !timeout_seconds.has_value()) {
        return invalid_config(
            "numeric configuration must be a positive in-range integer");
    }

    constexpr auto kInt64Max =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (max_tokens.value() > kInt64Max ||
        model_rounds.value() > std::numeric_limits<std::size_t>::max() ||
        tool_calls.value() > std::numeric_limits<std::size_t>::max() ||
        task_seconds.value() > kInt64Max / 1000 ||
        timeout_seconds.value() > kInt64Max / 1000) {
        return invalid_config("numeric configuration is out of range");
    }

    const auto runtime_root_value = environment.get("AGENT_RUNTIME_ROOT");
    if (runtime_root_value.has_value() && runtime_root_value->empty()) {
        return invalid_config("AGENT_RUNTIME_ROOT must not be empty");
    }
    const auto system_prompt = environment.get("AGENT_SYSTEM_PROMPT");

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

}  // namespace agent

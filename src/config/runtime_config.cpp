#include "config/runtime_config.h"

#include <dotenv.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <set>
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

bool path_is_link_or_reparse(const std::filesystem::path& path,
                             std::error_code& error) noexcept {
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || std::filesystem::is_symlink(status)) {
        return true;
    }
#if defined(_WIN32)
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = std::error_code(static_cast<int>(GetLastError()),
                                std::system_category());
        return true;
    }
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return false;
#endif
}

bool trusted_path_components(const std::filesystem::path& supplied) noexcept {
    try {
        std::error_code error;
        auto current = supplied.root_path();
        for (const auto& component : supplied.relative_path()) {
            current /= component;
            if (path_is_link_or_reparse(current, error) || error) {
                return false;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<nlohmann::json> read_strict_json_file(
    const std::filesystem::path& path) noexcept {
    try {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error) || error ||
            path_is_link_or_reparse(path, error) || error ||
            std::filesystem::hard_link_count(path, error) != 1 || error ||
            std::filesystem::file_size(path, error) > 65'536 || error) {
            return std::nullopt;
        }
        std::ifstream input(path, std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>()};
        if ((!input.good() && !input.eof()) || bytes.empty()) {
            return std::nullopt;
        }
        bool duplicate = false;
        std::vector<std::set<std::string>> keys;
        const nlohmann::json::parser_callback_t callback =
            [&](int depth, nlohmann::json::parse_event_t event,
                nlohmann::json& parsed) {
                if (event == nlohmann::json::parse_event_t::object_start) {
                    const auto index = static_cast<std::size_t>(depth + 1);
                    if (keys.size() <= index) {
                        keys.resize(index + 1);
                    }
                    keys[index].clear();
                } else if (event == nlohmann::json::parse_event_t::key) {
                    const auto index = static_cast<std::size_t>(depth);
                    if (keys.size() <= index) {
                        keys.resize(index + 1);
                    }
                    if (!keys[index].insert(parsed.get<std::string>()).second) {
                        duplicate = true;
                    }
                }
                return true;
            };
        auto value = nlohmann::json::parse(bytes, callback, true, false);
        if (value.is_discarded() || duplicate) {
            return std::nullopt;
        }
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

bool manifest_is_complete(const nlohmann::json& value) {
    static const std::set<std::string> expected{
        "schema_version", "pack_id", "snapshot_date", "document_count",
        "chunk_count", "embedding_model", "embedding_revision",
        "embedding_dimensions", "relevance_dense_min", "complete", "files"};
    if (!value.is_object() || value.size() != expected.size()) {
        return false;
    }
    for (const auto& key : expected) {
        if (!value.contains(key)) {
            return false;
        }
    }
    return value.at("schema_version").is_number_integer() &&
           value.at("schema_version").get<std::int64_t>() == 2 &&
           value.at("document_count").is_number_integer() &&
           value.at("document_count").get<std::int64_t>() == 30'000 &&
           value.at("embedding_model").is_string() &&
           value.at("embedding_model").get_ref<const std::string&>() ==
               "BAAI/bge-m3" &&
           value.at("embedding_revision").is_string() &&
           value.at("embedding_revision").get_ref<const std::string&>() ==
               "5617a9f61b028005a4858fdac845db406aefb181" &&
           value.at("embedding_dimensions").is_number_integer() &&
           value.at("embedding_dimensions").get<std::int64_t>() == 1'024 &&
           value.at("complete").is_boolean() &&
           value.at("complete").get<bool>();
}

std::optional<std::filesystem::path> trusted_pack_root(
    const std::filesystem::path& supplied) noexcept {
    try {
        if (supplied.empty() || !supplied.is_absolute() ||
            !trusted_path_components(supplied)) {
            return std::nullopt;
        }
        std::error_code error;
        const auto canonical = std::filesystem::canonical(supplied, error);
        if (error || !std::filesystem::is_directory(canonical, error) || error ||
            !trusted_path_components(canonical)) {
            return std::nullopt;
        }
        const auto manifest = read_strict_json_file(canonical / "pack.json");
        if (!manifest.has_value() || !manifest_is_complete(*manifest)) {
            return std::nullopt;
        }
        for (const auto& required : {
                 std::filesystem::path("runtime") / "python.exe",
                 std::filesystem::path("sidecar") / "agent_rag_cli.py"}) {
            const auto file = canonical / required;
            if (!std::filesystem::is_regular_file(file, error) || error ||
                path_is_link_or_reparse(file, error) || error ||
                std::filesystem::hard_link_count(file, error) != 1 || error) {
                return std::nullopt;
            }
        }
        return canonical;
    } catch (...) {
        return std::nullopt;
    }
}

std::string lower_component(const std::filesystem::path& value) {
    auto text = value.generic_u8string();
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char byte) {
        return static_cast<char>(std::tolower(byte));
    });
    return text;
}

bool inside_ready_directory(const std::filesystem::path& root) {
    std::string previous;
    for (const auto& component : root) {
        const auto current = lower_component(component);
        if (previous == "out" && current == "agentframework-ready") {
            return true;
        }
        previous = current;
    }
    return false;
}

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

Result<RuntimeConfig> load_runtime_config(
    const Environment& environment,
    const std::filesystem::path& executable_path) {
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

    RagConfig rag;
    rag.enabled = rag_enabled.value();
    if (rag_enabled.value()) {
        const auto mode = environment.get("AGENT_RAG_MODE");
        const auto device = environment.get("AGENT_RAG_DEVICE");
        rag.mode = mode.has_value() ? *mode : "hybrid";
        rag.device = device.has_value() ? *device : "auto";
        if (rag.mode != "hybrid" && rag.mode != "lexical") {
            return invalid_config("rag mode must be hybrid or lexical");
        }
        if (rag.device != "auto" && rag.device != "cuda" &&
            rag.device != "cpu") {
            return invalid_config("rag device must be auto, cuda, or cpu");
        }
        const auto top_k = positive_integer(environment, "AGENT_RAG_TOP_K", 6);
        const auto maximum = positive_integer(
            environment, "AGENT_RAG_MAX_TOTAL_BYTES", 32'768);
        const auto startup = positive_integer(
            environment, "AGENT_RAG_STARTUP_TIMEOUT_SECONDS", 120);
        const auto query = positive_integer(
            environment, "AGENT_RAG_QUERY_TIMEOUT_SECONDS", 30);
        if (!top_k.has_value() || !maximum.has_value() ||
            !startup.has_value() || !query.has_value()) {
            return invalid_config(
                "rag numeric configuration must be a positive bounded integer");
        }
        if (top_k.value() > 20) {
            return invalid_config("rag top-k must be between 1 and 20");
        }
        if (maximum.value() > 32'768) {
            return invalid_config("rag evidence budget must be at most 32768 bytes");
        }
        if (startup.value() > 600) {
            return invalid_config("rag startup timeout must be between 1 and 600 seconds");
        }
        if (query.value() > 120) {
            return invalid_config("rag query timeout must be between 1 and 120 seconds");
        }
        if (startup.value() >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 1000) ||
            query.value() >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 1000)) {
            return invalid_config("rag numeric configuration is out of range");
        }
        try {
            const auto supplied = environment.get("AGENT_RAG_PACK_ROOT");
            if (supplied.has_value()) {
                if (supplied->empty()) {
                    return invalid_config("AGENT_RAG_PACK_ROOT must not be empty");
                }
                const auto requested = std::filesystem::u8path(*supplied);
                if (!requested.is_absolute()) {
                    return invalid_config("AGENT_RAG_PACK_ROOT must be absolute");
                }
                const auto trusted = trusted_pack_root(requested);
                if (!trusted.has_value()) {
                    return invalid_config("rag knowledge pack is unavailable");
                }
                rag.pack_root = *trusted;
            } else {
                auto discovered = discover_rag_pack_root(executable_path);
                if (!discovered.has_value()) {
                    return Result<RuntimeConfig>::failure(discovered.error());
                }
                if (!discovered.value().has_value()) {
                    return invalid_config(
                        "enabled rag requires an external knowledge pack");
                }
                rag.pack_root = *discovered.value();
            }
            if (inside_ready_directory(rag.pack_root)) {
                return invalid_config("rag knowledge pack must be external");
            }
            rag.top_k = static_cast<std::size_t>(top_k.value());
            rag.max_total_bytes = static_cast<std::size_t>(maximum.value());
            rag.startup_timeout_ms =
                static_cast<std::int64_t>(startup.value() * 1000);
            rag.query_timeout_ms =
                static_cast<std::int64_t>(query.value() * 1000);
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

Result<std::optional<std::filesystem::path>> discover_rag_pack_root(
    const std::filesystem::path& executable_path) {
    try {
        if (executable_path.empty()) {
            return Result<std::optional<std::filesystem::path>>::success(
                std::nullopt);
        }
        std::error_code error;
        const auto executable = std::filesystem::absolute(
            executable_path, error).lexically_normal();
        if (error || executable.parent_path().empty()) {
            return Result<std::optional<std::filesystem::path>>::failure(
                {ErrorCode::InvalidConfiguration,
                 "rag active pack pointer is invalid", false});
        }
        const auto pointer = executable.parent_path().parent_path() /
                             "AgentFramework-Knowledge" / "active-pack.json";
        if (!std::filesystem::exists(pointer, error)) {
            if (error) {
                return Result<std::optional<std::filesystem::path>>::failure(
                    {ErrorCode::InvalidConfiguration,
                     "rag active pack pointer is invalid", false});
            }
            return Result<std::optional<std::filesystem::path>>::success(
                std::nullopt);
        }
        const auto value = read_strict_json_file(pointer);
        if (!value.has_value() || !value->is_object() || value->size() != 2 ||
            !value->contains("schema_version") ||
            !value->contains("pack_root") ||
            !value->at("schema_version").is_number_integer() ||
            value->at("schema_version").get<std::int64_t>() != 1 ||
            !value->at("pack_root").is_string()) {
            return Result<std::optional<std::filesystem::path>>::failure(
                {ErrorCode::InvalidConfiguration,
                 "rag active pack pointer is invalid", false});
        }
        const auto supplied =
            std::filesystem::u8path(value->at("pack_root").get<std::string>());
        if (!supplied.is_absolute()) {
            return Result<std::optional<std::filesystem::path>>::failure(
                {ErrorCode::InvalidConfiguration,
                 "rag active pack pointer is invalid", false});
        }
        const auto trusted = trusted_pack_root(supplied);
        if (!trusted.has_value() || inside_ready_directory(*trusted)) {
            return Result<std::optional<std::filesystem::path>>::failure(
                {ErrorCode::InvalidConfiguration,
                 "rag active pack pointer is invalid", false});
        }
        return Result<std::optional<std::filesystem::path>>::success(*trusted);
    } catch (...) {
        return Result<std::optional<std::filesystem::path>>::failure(
            {ErrorCode::InvalidConfiguration,
             "rag active pack pointer is invalid", false});
    }
}

}  // namespace agent

#include "adapters/workspace/workspace_path_policy.h"

#include "adapters/workspace/workspace_text.h"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>

namespace agent::workspace {
namespace {

Fault invalid_path() {
    return {FaultCode::InvalidArguments,
            "invalid workspace-relative path", false};
}

Fault denied_path() {
    return {FaultCode::AccessDenied, "workspace path is not permitted", false};
}

Fault missing_path() {
    return {FaultCode::NotFound, "workspace path was not found", false};
}

Fault unsupported_path() {
    return {FaultCode::UnsupportedFile,
            "workspace object has an unsupported type", false};
}

Fault io_path() {
    return {FaultCode::IoError, "workspace path inspection failed", false};
}

std::string ascii_lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char value) {
        if (value >= static_cast<unsigned char>('A') &&
            value <= static_cast<unsigned char>('Z')) {
            return static_cast<char>(value + ('a' - 'A'));
        }
        return static_cast<char>(value);
    });
    return text;
}

bool is_windows_reserved(std::string component) {
    const auto dot = component.find('.');
    if (dot != std::string::npos) {
        component.resize(dot);
    }
    component = ascii_lower(std::move(component));
    if (component == "con" || component == "prn" || component == "aux" ||
        component == "nul") {
        return true;
    }
    if (component.size() == 4 &&
        (component.rfind("com", 0) == 0 || component.rfind("lpt", 0) == 0) &&
        component[3] >= '1' && component[3] <= '9') {
        return true;
    }
    return false;
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

}  // namespace

WorkspacePathPolicy::WorkspacePathPolicy(std::filesystem::path runtime_root)
    : runtime_root_(std::move(runtime_root)) {}

Outcome<RelativePath> WorkspacePathPolicy::parse(std::string_view text) const {
    if (text.empty() || text.size() > 4096 || !is_strict_utf8_text(text) ||
        text.front() == '/' || text.back() == '/' ||
        text.find('\\') != std::string_view::npos ||
        (text.size() >= 2 &&
         ((text[0] >= 'A' && text[0] <= 'Z') ||
          (text[0] >= 'a' && text[0] <= 'z')) &&
         text[1] == ':')) {
        return invalid_path();
    }
    if (text == ".") {
        return RelativePath{".", {}};
    }

    RelativePath parsed{std::string(text), {}};
    std::size_t begin = 0;
    while (begin < text.size()) {
        const auto separator = text.find('/', begin);
        const auto end = separator == std::string_view::npos
                             ? text.size()
                             : separator;
        const auto part = text.substr(begin, end - begin);
        if (part.empty() || part == "." || part == ".." || part.size() > 255) {
            return invalid_path();
        }
#if defined(_WIN32)
        if (part.find(':') != std::string_view::npos ||
            part.back() == '.' || part.back() == ' ' ||
            is_windows_reserved(std::string(part))) {
            return invalid_path();
        }
#endif
        parsed.components.emplace_back(part);
        if (separator == std::string_view::npos) {
            break;
        }
        begin = separator + 1;
    }
    return parsed;
}

bool WorkspacePathPolicy::is_protected(
    const std::filesystem::path& workspace,
    const RelativePath& relative) const {
    for (const auto& raw_component : relative.components) {
        const auto component = ascii_lower(raw_component);
        if (component == ".git" || component == ".worktrees" ||
            component == "runtime_data") {
            return true;
        }
    }
    if (!relative.components.empty()) {
        const auto leaf = ascii_lower(relative.components.back());
        const bool env_file = leaf == ".env" ||
                              (leaf.rfind(".env.", 0) == 0 &&
                               leaf != ".env.example");
        const bool credential =
            leaf == ".git-credentials" || leaf == ".netrc" ||
            leaf == "_netrc" || leaf == ".npmrc" || leaf == ".pypirc";
        const bool temporary = leaf.rfind(".agent-tmp-", 0) == 0;
        if (env_file || credential || temporary) {
            return true;
        }
    }

    std::error_code error;
    const auto workspace_absolute =
        std::filesystem::absolute(workspace, error).lexically_normal();
    if (error) {
        return true;
    }
    const auto runtime_absolute =
        std::filesystem::absolute(runtime_root_, error).lexically_normal();
    if (error || !contained(workspace_absolute, runtime_absolute)) {
        return false;
    }
    const auto candidate = relative.components.empty()
                               ? workspace_absolute
                               : (workspace_absolute /
                                  std::filesystem::u8path(relative.generic))
                                     .lexically_normal();
    return path_equal(candidate, runtime_absolute) ||
           contained(runtime_absolute, candidate);
}

Outcome<std::filesystem::path> WorkspacePathPolicy::resolve_existing(
    const std::filesystem::path& workspace,
    const RelativePath& relative,
    bool require_directory) const {
    if (is_protected(workspace, relative)) {
        return denied_path();
    }
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(
        std::filesystem::absolute(workspace, error), error);
    if (error || !std::filesystem::exists(root, error) || error) {
        return missing_path();
    }
    if (!std::filesystem::is_directory(root, error) || error) {
        return unsupported_path();
    }
    if (is_reparse_or_link(root, error)) {
        return denied_path();
    }
    if (error) {
        return io_path();
    }

    auto current = root;
    for (const auto& component : relative.components) {
        current /= std::filesystem::u8path(component);
        if (!std::filesystem::exists(current, error) || error) {
            return missing_path();
        }
        if (is_reparse_or_link(current, error)) {
            return denied_path();
        }
        if (error) {
            return io_path();
        }
    }
    const auto candidate = std::filesystem::weakly_canonical(current, error);
    if (error || !contained(root, candidate)) {
        return denied_path();
    }
    if (require_directory) {
        if (!std::filesystem::is_directory(candidate, error) || error) {
            return unsupported_path();
        }
        return candidate;
    }
    if (!std::filesystem::is_regular_file(candidate, error) || error) {
        return unsupported_path();
    }
    const auto links = std::filesystem::hard_link_count(candidate, error);
    if (error) {
        return io_path();
    }
    if (links != 1) {
        return denied_path();
    }
    return candidate;
}

Outcome<std::filesystem::path> WorkspacePathPolicy::resolve_parent(
    const std::filesystem::path& workspace,
    const RelativePath& relative) const {
    if (relative.components.empty() || is_protected(workspace, relative)) {
        return denied_path();
    }
    RelativePath parent{".", {}};
    if (relative.components.size() > 1) {
        parent.components.assign(relative.components.begin(),
                                 relative.components.end() - 1);
        parent.generic.clear();
        for (std::size_t index = 0; index < parent.components.size(); ++index) {
            if (index != 0) {
                parent.generic.push_back('/');
            }
            parent.generic += parent.components[index];
        }
    }
    return resolve_existing(workspace, parent, true);
}

}  // namespace agent::workspace

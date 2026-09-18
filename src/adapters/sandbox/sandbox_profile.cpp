#include "ports/sandbox.h"

#include <algorithm>
#include <utility>

namespace agent {
namespace {

bool is_truthy(std::string_view value) {
    return value == "1" || value == "true" || value == "yes" ||
           value == "on" || value == "TRUE" || value == "True";
}

bool is_falsy(std::string_view value) {
    return value == "0" || value == "false" || value == "no" ||
           value == "off" || value == "FALSE" || value == "False";
}

std::vector<std::string> split_csv(std::string_view raw) {
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (begin <= raw.size()) {
        const auto comma = raw.find(',', begin);
        const auto end = comma == std::string_view::npos ? raw.size() : comma;
        std::string piece(raw.substr(begin, end - begin));
        std::size_t a = 0;
        while (a < piece.size() &&
               std::isspace(static_cast<unsigned char>(piece[a]))) {
            ++a;
        }
        std::size_t b = piece.size();
        while (b > a &&
               std::isspace(static_cast<unsigned char>(piece[b - 1]))) {
            --b;
        }
        piece = piece.substr(a, b - a);
        if (!piece.empty()) {
            parts.push_back(std::move(piece));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        begin = comma + 1;
    }
    return parts;
}

}  // namespace

const char* sandbox_fault_code_name(SandboxFaultCode code) {
    switch (code) {
        case SandboxFaultCode::InvalidConfiguration:
            return "invalid_configuration";
        case SandboxFaultCode::UnsupportedPlatform:
            return "unsupported_platform";
        case SandboxFaultCode::DependencyUnavailable:
            return "dependency_unavailable";
        case SandboxFaultCode::LimitExceeded:
            return "limit_exceeded";
    }
    return "invalid_configuration";
}

SandboxProfile parse_sandbox_profile(
    const std::map<std::string, std::string>& fields) {
    SandboxProfile profile;
    const auto find = [&](const char* key) {
        const auto it = fields.find(key);
        return it == fields.end() ? std::string_view{} : std::string_view{it->second};
    };
    const auto deny_network = find("deny_network");
    if (!deny_network.empty()) {
        profile.deny_network = is_truthy(deny_network) ||
                               (!is_falsy(deny_network) && deny_network != "");
    }
    const auto deny_writes = find("deny_writes_outside_workspace");
    if (!deny_writes.empty()) {
        profile.deny_writes_outside_workspace =
            is_truthy(deny_writes) ||
            (!is_falsy(deny_writes) && deny_writes != "");
    }
    const auto read_paths = find("read_paths");
    if (!read_paths.empty()) {
        profile.read_paths = split_csv(read_paths);
    }
    const auto write_paths = find("write_paths");
    if (!write_paths.empty()) {
        profile.write_paths = split_csv(write_paths);
    }
    return profile;
}

std::map<std::string, std::string> serialise_sandbox_profile(
    const SandboxProfile& profile) {
    std::map<std::string, std::string> out;
    out["deny_network"] = profile.deny_network ? "true" : "false";
    out["deny_writes_outside_workspace"] =
        profile.deny_writes_outside_workspace ? "true" : "false";
    auto join = [](const std::vector<std::string>& items) {
        std::string text;
        for (std::size_t index = 0; index < items.size(); ++index) {
            if (index != 0) text.push_back(',');
            text += items[index];
        }
        return text;
    };
    out["read_paths"] = join(profile.read_paths);
    out["write_paths"] = join(profile.write_paths);
    if (profile.max_cpu_time_ms.has_value()) {
        out["max_cpu_time_ms"] = std::to_string(*profile.max_cpu_time_ms);
    }
    if (profile.max_memory_bytes.has_value()) {
        out["max_memory_bytes"] = std::to_string(*profile.max_memory_bytes);
    }
    return out;
}

}  // namespace agent

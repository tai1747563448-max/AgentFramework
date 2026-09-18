#include "services/dangerous_patterns.h"
#include "domain/value.h"

#include <cctype>
#include <string>
#include <vector>

namespace agent {
namespace {

std::string lowered(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char byte : text) {
        out.push_back(static_cast<char>(std::tolower(
            static_cast<unsigned char>(byte))));
    }
    return out;
}

bool has_drive_prefix(const std::string& text) {
    // "C:\" / "C:/" or any character followed by ':' + slash.
    if (text.size() < 3) return false;
    return std::isalpha(static_cast<unsigned char>(text[0])) != 0 &&
           text[1] == ':' &&
           (text[2] == '/' || text[2] == '\\');
}

bool rooted_around(const std::string& haystack_lower,
                   std::size_t hit_position,
                   std::size_t needle_size) {
    // The matched needle sits at haystack_lower[hit_position ..
    // hit_position + needle_size). Walk back to find the start of the
    // path component and check whether it begins with '/', '\', or a
    // Windows drive prefix.
    std::size_t path_start = 0;
    for (std::size_t index = hit_position; index > 0; --index) {
        const char byte = haystack_lower[index - 1];
        if (byte == ' ' || byte == '"' || byte == '\n' || byte == '\r') {
            path_start = index;
            break;
        }
    }
    if (path_start >= haystack_lower.size()) return true;
    const char first = haystack_lower[path_start];
    if (first == '/' || first == '\\') return true;
    if (has_drive_prefix(haystack_lower.substr(path_start))) return true;
    // Allow tilde-prefixed paths (~/.ssh, ~/.aws) by accepting a
    // leading '~' before the slash. The CLI renders workspace paths
    // as cwd-relative; user home expansions stay on the agent host.
    if (first == '~' && path_start + 1 < haystack_lower.size()) {
        const char next = haystack_lower[path_start + 1];
        return next == '/' || next == '\\';
    }
    return false;
}

}  // namespace

const DangerousPattern* match_dangerous_pattern(
    const std::string& tool_name,
    const Value& input_value) {
    // Walk the Value recursively and concatenate every string leaf.
    // We do not need a faithful JSON serialiser — the pattern matcher
    // only cares about whether a needle substring sits inside any
    // string field (paths, commands, URLs). Nested arrays / objects
    // contribute their string children, separated by spaces so the
    // matcher does not accidentally glue two fields together.
    std::string flattened;
    std::vector<const Value*> stack{&input_value};
    while (!stack.empty()) {
        const Value* node = stack.back();
        stack.pop_back();
        if (node == nullptr) continue;
        const auto& storage = node->storage();
        if (std::holds_alternative<std::string>(storage)) {
            if (!flattened.empty()) flattened.push_back(' ');
            flattened += std::get<std::string>(storage);
        } else if (std::holds_alternative<
                       std::shared_ptr<const Value::ArrayNode>>(storage)) {
            const auto& array = std::get<
                std::shared_ptr<const Value::ArrayNode>>(storage)->values;
            for (const auto& child : array) {
                stack.push_back(&child);
            }
        } else if (std::holds_alternative<
                       std::shared_ptr<const Value::ObjectNode>>(storage)) {
            const auto& object = std::get<
                std::shared_ptr<const Value::ObjectNode>>(storage)->values;
            for (const auto& [key, child] : object) {
                if (!flattened.empty()) flattened.push_back(' ');
                flattened += key;
                stack.push_back(&child);
            }
        } else if (std::holds_alternative<std::int64_t>(storage)) {
            flattened += std::to_string(std::get<std::int64_t>(storage));
            flattened.push_back(' ');
        } else if (std::holds_alternative<double>(storage)) {
            flattened += std::to_string(std::get<double>(storage));
            flattened.push_back(' ');
        }
    }
    return match_dangerous_pattern(tool_name, flattened);
}

const DangerousPattern* match_dangerous_pattern(
    const std::string& tool_name,
    const std::string& input_json) {
    // Normalise backslashes to forward slashes so the same needle
    // matches POSIX paths ("/home/u/.aws/") and Windows paths
    // ("C:\\Users\\me\\.aws\\"). The lowered comparison folds both
    // cases together so the haystack stays one string.
    std::string normalised = input_json;
    for (char& byte : normalised) {
        if (byte == '\\') byte = '/';
    }
    const auto lower_name = lowered(tool_name);
    const auto lower_input = lowered(normalised);
    for (const auto& pattern : dangerous_pattern_table()) {
        const auto lower_needle = lowered(pattern.needle);
        // Combine name + input so "rm" in tool name plus " -rf /" in
        // payload still trips the destructive pattern. A bare mention
        // of "curl" in the body is not enough — it must combine with
        // a shell-pipe to sh.
        const std::string haystack = lower_name + "\n" + lower_input;
        std::size_t cursor = 0;
        while (cursor < haystack.size()) {
            const auto hit = haystack.find(lower_needle, cursor);
            if (hit == std::string::npos) break;
            if (!pattern.require_rooted ||
                rooted_around(haystack, hit, lower_needle.size())) {
                return &pattern;
            }
            cursor = hit + 1;
        }
        // Special-case curl | sh: "curl" must be followed (anywhere on
        // the line) by "sh" or "bash" to count as the textbook chain.
        if (pattern.id == std::string("curl_pipe_sh")) {
            const auto& curl_haystack = haystack;
            std::size_t scan = 0;
            while (scan < curl_haystack.size()) {
                const auto hit = curl_haystack.find("curl", scan);
                if (hit == std::string::npos) break;
                const auto rest = curl_haystack.substr(hit);
                if (rest.find('|') != std::string::npos &&
                    (rest.find("sh") != std::string::npos ||
                     rest.find("bash") != std::string::npos)) {
                    return &pattern;
                }
                scan = hit + 1;
            }
        }
    }
    return nullptr;
}

}  // namespace agent
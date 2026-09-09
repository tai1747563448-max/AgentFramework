#include "application/memory_policy.h"

#include <algorithm>
#include <cctype>
#include <cstdint>

namespace agent {
namespace {

bool decode_utf8(std::string_view text, std::vector<std::uint32_t>* points) {
    if (points != nullptr)
        points->clear();
    for (std::size_t offset = 0; offset < text.size();) {
        const auto first = static_cast<unsigned char>(text[offset]);
        if (first == 0)
            return false;
        std::size_t width = 0;
        std::uint32_t point = 0;
        if (first <= 0x7f) {
            width = 1;
            point = first;
        } else if (first >= 0xc2 && first <= 0xdf) {
            width = 2;
            point = first & 0x1f;
        } else if (first >= 0xe0 && first <= 0xef) {
            width = 3;
            point = first & 0x0f;
        } else if (first >= 0xf0 && first <= 0xf4) {
            width = 4;
            point = first & 0x07;
        } else {
            return false;
        }
        if (offset + width > text.size())
            return false;
        for (std::size_t index = 1; index < width; ++index) {
            const auto byte = static_cast<unsigned char>(text[offset + index]);
            if ((byte & 0xc0) != 0x80)
                return false;
            point = (point << 6) | (byte & 0x3f);
        }
        if ((width == 3 && point < 0x800) || (width == 4 && point < 0x10000) ||
            (point >= 0xd800 && point <= 0xdfff) || point > 0x10ffff)
            return false;
        if (points != nullptr)
            points->push_back(point);
        offset += width;
    }
    return true;
}

bool whitespace(std::uint32_t point) {
    return point == ' ' || (point >= '\t' && point <= '\r') || point == 0x0085 ||
           point == 0x00a0 || point == 0x1680 ||
           (point >= 0x2000 && point <= 0x200a) || point == 0x2028 ||
           point == 0x2029 || point == 0x202f || point == 0x205f ||
           point == 0x3000;
}

std::string lowercase_ascii(std::string_view text) {
    std::string lower(text);
    for (auto& character : lower) {
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    }
    return lower;
}

bool token_character(char character) {
    const auto value = static_cast<unsigned char>(character);
    return std::isalnum(value) != 0 || character == '-' || character == '_';
}

bool name_boundary_character(char character) {
    return std::isalnum(static_cast<unsigned char>(character)) != 0;
}

bool ascii_letter(char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z');
}

bool starts_with_word(const std::string& lower, std::size_t value,
                      std::string_view word) {
    const auto end = value + word.size();
    return end <= lower.size() && lower.compare(value, word.size(), word) == 0 &&
           (end == lower.size() ||
            std::isspace(static_cast<unsigned char>(lower[end])));
}

bool contains_word(const std::string& lower, std::size_t value,
                   std::string_view word) {
    for (auto start = lower.find(word, value); start != std::string::npos;
         start = lower.find(word, start + 1)) {
        const auto end = start + word.size();
        if ((start == value || !ascii_letter(lower[start - 1])) &&
            (end == lower.size() || !ascii_letter(lower[end])))
            return true;
    }
    return false;
}

bool has_policy_semantics(const std::string& lower, std::size_t value) {
    return contains_word(lower, value, "length") ||
           contains_word(lower, value, "character") ||
           contains_word(lower, value, "characters") ||
           contains_word(lower, value, "phrase") ||
           contains_word(lower, value, "passphrase");
}

bool colon_value_is_policy_prose(const std::string& lower, std::size_t value) {
    if (starts_with_word(lower, value, "should") ||
        starts_with_word(lower, value, "must"))
        return has_policy_semantics(lower, value);
    if (starts_with_word(lower, value, "recommended"))
        return contains_word(lower, value, "length") &&
               has_policy_semantics(lower, value);
    if (starts_with_word(lower, value, "use"))
        return contains_word(lower, value, "at") &&
               contains_word(lower, value, "least") &&
               has_policy_semantics(lower, value);
    if (starts_with_word(lower, value, "prefer"))
        return contains_word(lower, value, "long") &&
               (contains_word(lower, value, "phrase") ||
                contains_word(lower, value, "passphrase"));
    return false;
}

bool has_long_sk_token(const std::string& lower) {
    for (std::size_t start = 0; start + 3 <= lower.size(); ++start) {
        if (lower[start] != 's' || lower[start + 1] != 'k' ||
            (lower[start + 2] != '-' && lower[start + 2] != '_'))
            continue;
        std::size_t end = start + 3;
        while (end < lower.size() && token_character(lower[end]))
            ++end;
        if (end - (start + 3) >= 16)
            return true;
    }
    return false;
}

bool has_bearer_token(const std::string& lower) {
    constexpr std::string_view prefix = "bearer";
    for (std::size_t start = lower.find(prefix); start != std::string::npos;
         start = lower.find(prefix, start + 1)) {
        const bool left_boundary = start == 0 || !token_character(lower[start - 1]);
        const auto after = start + prefix.size();
        if (!left_boundary || after >= lower.size() ||
            !std::isspace(static_cast<unsigned char>(lower[after])))
            continue;
        std::size_t value = after;
        while (value < lower.size() &&
               std::isspace(static_cast<unsigned char>(lower[value])))
            ++value;
        std::size_t end = value;
        while (end < lower.size() && token_character(lower[end]))
            ++end;
        if (end - value >= 12)
            return true;
    }
    return false;
}

bool has_assignment_value(const std::string& lower, std::string_view name) {
    for (std::size_t start = lower.find(name); start != std::string::npos;
         start = lower.find(name, start + 1)) {
        const auto after = start + name.size();
        if ((start != 0 && name_boundary_character(lower[start - 1])) ||
            (after < lower.size() && name_boundary_character(lower[after])))
            continue;
        std::size_t value = after;
        if (value < lower.size() &&
            (lower[value] == '\'' || lower[value] == '"'))
            ++value;
        while (value < lower.size() &&
               std::isspace(static_cast<unsigned char>(lower[value])))
            ++value;
        if (value == lower.size() || (lower[value] != '=' && lower[value] != ':'))
            continue;
        const char separator = lower[value++];
        while (value < lower.size() &&
               std::isspace(static_cast<unsigned char>(lower[value])))
            ++value;
        if (separator == ':') {
            if (colon_value_is_policy_prose(lower, value))
                continue;
            // Check the whole colon value; credentials may contain spaces.
            std::size_t value_length = 0;
            for (std::size_t index = value; index < lower.size(); ++index) {
                if (!std::isspace(static_cast<unsigned char>(lower[index])))
                    ++value_length;
            }
            if (value_length >= 4)
                return true;
            continue;
        }
        std::size_t end = value;
        while (end < lower.size() && !std::isspace(static_cast<unsigned char>(lower[end])))
            ++end;
        if (end - value >= 4)
            return true;
    }
    return false;
}

bool looks_sensitive(const std::string& lower) {
    return has_long_sk_token(lower) || has_bearer_token(lower) ||
           has_assignment_value(lower, "password") ||
           has_assignment_value(lower, "secret") ||
           has_assignment_value(lower, "api-key") ||
           has_assignment_value(lower, "api_key") ||
           has_assignment_value(lower, "apikey") ||
           (lower.find("-----begin ") != std::string::npos &&
            lower.find(" private key-----") != std::string::npos);
}

Result<void> rejected() {
    return Result<void>::failure(
        {ErrorCode::InvalidInput, "memory candidate is not safe to persist", false});
}

} // namespace

MemoryPolicy::MemoryPolicy(MemoryPolicyConfig config) : config_(std::move(config)) {}

Result<void> MemoryPolicy::validate_candidate(std::string_view content) const {
    std::vector<std::uint32_t> points;
    if (!decode_utf8(content, &points) || content.size() > config_.max_entry_bytes ||
        points.empty() ||
        std::all_of(points.begin(), points.end(), [](std::uint32_t point) {
            return whitespace(point);
        }))
        return rejected();
    for (const auto& protected_value : config_.protected_values) {
        if (!protected_value.empty() && content.find(protected_value) != std::string_view::npos)
            return rejected();
    }
    if (looks_sensitive(lowercase_ascii(content)))
        return rejected();
    return Result<void>::success();
}

bool memory_opted_out(std::string_view text) {
    const std::string lower = lowercase_ascii(text);
    for (const auto phrase : {std::string_view{"不要使用记忆"},
                              std::string_view{"不要参考记忆"},
                              std::string_view{"别参考历史"},
                              std::string_view{"忽略之前的记忆"},
                              std::string_view{"do not use memory"},
                              std::string_view{"don't use memory"},
                              std::string_view{"ignore previous memory"},
                              std::string_view{"ignore memories"}}) {
        if (lower.find(phrase) != std::string::npos)
            return true;
    }
    return false;
}

} // namespace agent

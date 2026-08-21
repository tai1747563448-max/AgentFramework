#include "domain/evidence_validation.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace agent {
namespace {

constexpr std::size_t kMaxEvidenceItems = 20;
constexpr std::size_t kMaxSourceIdBytes = 512;
constexpr std::size_t kMaxContentBytes = 8'192;
constexpr std::size_t kMaxTotalContentBytes = 32'768;
constexpr std::size_t kMaxMetadataDepth = 16;
constexpr std::size_t kMaxMetadataNodes = 256;
constexpr std::size_t kMaxMetadataStringBytes = 4'096;

bool is_continuation(std::uint8_t byte) noexcept {
    return byte >= 0x80U && byte <= 0xBFU;
}

bool is_valid_utf8(const std::string& text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = static_cast<std::uint8_t>(text[index]);
        if (first <= 0x7FU) {
            ++index;
            continue;
        }

        if (first >= 0xC2U && first <= 0xDFU) {
            if (index + 1 >= text.size() ||
                !is_continuation(static_cast<std::uint8_t>(text[index + 1]))) {
                return false;
            }
            index += 2;
            continue;
        }

        if (first >= 0xE0U && first <= 0xEFU) {
            if (index + 2 >= text.size()) {
                return false;
            }
            const auto second = static_cast<std::uint8_t>(text[index + 1]);
            const auto third = static_cast<std::uint8_t>(text[index + 2]);
            const bool second_is_valid =
                first == 0xE0U ? second >= 0xA0U && second <= 0xBFU
                : first == 0xEDU ? second >= 0x80U && second <= 0x9FU
                                 : is_continuation(second);
            if (!second_is_valid || !is_continuation(third)) {
                return false;
            }
            index += 3;
            continue;
        }

        if (first >= 0xF0U && first <= 0xF4U) {
            if (index + 3 >= text.size()) {
                return false;
            }
            const auto second = static_cast<std::uint8_t>(text[index + 1]);
            const auto third = static_cast<std::uint8_t>(text[index + 2]);
            const auto fourth = static_cast<std::uint8_t>(text[index + 3]);
            const bool second_is_valid =
                first == 0xF0U ? second >= 0x90U && second <= 0xBFU
                : first == 0xF4U ? second >= 0x80U && second <= 0x8FU
                                 : is_continuation(second);
            if (!second_is_valid || !is_continuation(third) ||
                !is_continuation(fourth)) {
                return false;
            }
            index += 4;
            continue;
        }

        return false;
    }
    return true;
}

bool contains_nul(const std::string& text) noexcept {
    return text.find('\0') != std::string::npos;
}

bool source_id_is_valid(const std::string& source_id) noexcept {
    if (source_id.empty() || source_id.size() > kMaxSourceIdBytes ||
        !is_valid_utf8(source_id)) {
        return false;
    }
    for (const unsigned char byte : source_id) {
        if (byte < 0x20U || byte == 0x7FU) {
            return false;
        }
    }
    return true;
}

struct MetadataBudget {
    std::size_t nodes{0};
    std::size_t string_bytes{0};
};

bool add_metadata_string(const std::string& text,
                         MetadataBudget& budget) noexcept {
    if (contains_nul(text) || !is_valid_utf8(text) ||
        text.size() > kMaxMetadataStringBytes - budget.string_bytes) {
        return false;
    }
    budget.string_bytes += text.size();
    return true;
}

bool metadata_is_valid(const Value& value, std::size_t depth,
                       MetadataBudget& budget) noexcept {
    if (depth > kMaxMetadataDepth || budget.nodes >= kMaxMetadataNodes) {
        return false;
    }
    ++budget.nodes;

    const auto& storage = value.storage();
    if (std::holds_alternative<std::nullptr_t>(storage) ||
        std::holds_alternative<bool>(storage) ||
        std::holds_alternative<std::int64_t>(storage)) {
        return true;
    }
    if (const auto* number = std::get_if<double>(&storage)) {
        return std::isfinite(*number);
    }
    if (const auto* text = std::get_if<std::string>(&storage)) {
        return add_metadata_string(*text, budget);
    }
    if (const auto* array =
            std::get_if<std::shared_ptr<const Value::ArrayNode>>(&storage)) {
        if (*array == nullptr) {
            return false;
        }
        for (const auto& item : (*array)->values) {
            if (!metadata_is_valid(item, depth + 1, budget)) {
                return false;
            }
        }
        return true;
    }

    const auto* object =
        std::get_if<std::shared_ptr<const Value::ObjectNode>>(&storage);
    if (object == nullptr || *object == nullptr) {
        return false;
    }
    for (const auto& entry : (*object)->values) {
        if (!add_metadata_string(entry.first, budget) ||
            !metadata_is_valid(entry.second, depth + 1, budget)) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool evidence_pack_is_valid(const EvidencePack& evidence) noexcept {
    if (evidence.items.size() > kMaxEvidenceItems) {
        return false;
    }

    std::size_t total_content_bytes = 0;
    for (std::size_t index = 0; index < evidence.items.size(); ++index) {
        const auto& item = evidence.items[index];
        if (!source_id_is_valid(item.source_id)) {
            return false;
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (evidence.items[previous].source_id == item.source_id) {
                return false;
            }
        }

        if (item.content.empty() || item.content.size() > kMaxContentBytes ||
            contains_nul(item.content) || !is_valid_utf8(item.content) ||
            item.content.size() >
                kMaxTotalContentBytes - total_content_bytes) {
            return false;
        }
        total_content_bytes += item.content.size();

        MetadataBudget metadata_budget;
        if (!metadata_is_valid(item.metadata, 1, metadata_budget)) {
            return false;
        }
    }
    return true;
}

}  // namespace agent

#include "domain/memory_event.h"
#include "domain/task_state.h"

#include <algorithm>
#include <type_traits>

namespace agent {
namespace {

bool valid_text(const std::string &text, bool allow_empty = false) {
    if (!allow_empty &&
        (text.empty() || text.find_first_not_of(" \t\r\n") == std::string::npos))
        return false;
    // Validate strict UTF-8 at the domain boundary.
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
        } else
            return false;
        if (offset + width > text.size())
            return false;
        for (std::size_t i = 1; i < width; ++i) {
            const auto byte = static_cast<unsigned char>(text[offset + i]);
            if ((byte & 0xc0) != 0x80)
                return false;
            point = (point << 6) | (byte & 0x3f);
        }
        if ((width == 3 && point < 0x800) || (width == 4 && point < 0x10000) ||
            (point >= 0xd800 && point <= 0xdfff) || point > 0x10ffff)
            return false;
        offset += width;
    }
    return true;
}

bool valid_timestamp(const std::string &time) {
    if (time.size() != 20 && time.size() != 24)
        return false;
    for (std::size_t i = 0; i < time.size(); ++i) {
        if (i == 4 || i == 7) {
            if (time[i] != '-')
                return false;
        } else if (i == 10) {
            if (time[i] != 'T')
                return false;
        } else if (i == 13 || i == 16) {
            if (time[i] != ':')
                return false;
        } else if (i == time.size() - 1) {
            if (time[i] != 'Z')
                return false;
        } else if (i == 19) {
            if (time[i] != '.')
                return false;
        } else if (time[i] < '0' || time[i] > '9')
            return false;
    }
    const auto number = [&time](std::size_t offset, std::size_t length) {
        unsigned result = 0;
        for (std::size_t i = 0; i < length; ++i)
            result = result * 10 + (time[offset + i] - '0');
        return result;
    };
    const auto year = number(0, 4), month = number(5, 2), day = number(8, 2);
    if (year == 0 || month == 0 || month > 12 || day == 0 || number(11, 2) > 23 ||
        number(14, 2) > 59 || number(17, 2) > 59)
        return false;
    const unsigned days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    return day <= days[month - 1] + ((month == 2 && leap) ? 1U : 0U);
}

bool valid_entry(const MemoryEntry &entry, const std::string &event_time) {
    const bool category = entry.category == MemoryCategory::Preference ||
                          entry.category == MemoryCategory::Decision ||
                          entry.category == MemoryCategory::Fact ||
                          entry.category == MemoryCategory::Workflow ||
                          entry.category == MemoryCategory::Constraint;
    return is_valid_memory_id(entry.memory_id) && category &&
           (entry.origin == MemoryOrigin::ExplicitUser ||
            entry.origin == MemoryOrigin::ModelConsolidation) &&
           (entry.scope_utf8.empty() || valid_text(entry.scope_utf8)) &&
           valid_text(entry.content) && is_valid_session_id(entry.source_session_id) &&
           entry.source_turn_start > 0 &&
           entry.source_turn_end >= entry.source_turn_start &&
           valid_timestamp(entry.created_at_utc) &&
           valid_timestamp(entry.updated_at_utc) &&
           normalized_memory_timestamp(entry.created_at_utc) <=
               normalized_memory_timestamp(entry.updated_at_utc) &&
           normalized_memory_timestamp(entry.updated_at_utc) <=
               normalized_memory_timestamp(event_time);
}

} // namespace

MemoryEventKind memory_event_kind(const MemoryEventPayload &payload) {
    return std::visit(
        [](const auto &typed) {
            using T = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<T, MemoryUpsertedPayload>)
                return MemoryEventKind::MemoryUpserted;
            else if constexpr (std::is_same_v<T, MemoryForgottenPayload>)
                return MemoryEventKind::MemoryForgotten;
            else
                return MemoryEventKind::SessionMemoryConsolidated;
        },
        payload);
}

bool is_valid_memory_id(std::string_view id) noexcept {
    constexpr std::string_view prefix = "memory-";
    return id.size() == prefix.size() + 32 && id.substr(0, prefix.size()) == prefix &&
           std::all_of(id.begin() + prefix.size(), id.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

std::string normalized_memory_timestamp(const std::string &timestamp) {
    return timestamp.size() == 20 ? timestamp.substr(0, 19) + ".000Z" : timestamp;
}

Result<void> validate_memory_event(const MemoryEvent &event) {
    const bool envelope = event.schema_version == 1 && event.sequence > 0 &&
                          valid_timestamp(event.timestamp_utc) &&
                          valid_text(event.correlation_id);
    const bool payload =
        envelope &&
        std::visit(
            [&event](const auto &typed) {
                using T = std::decay_t<decltype(typed)>;
                if constexpr (std::is_same_v<T, MemoryUpsertedPayload>)
                    return valid_entry(typed.entry, event.timestamp_utc);
                else if constexpr (std::is_same_v<T, MemoryForgottenPayload>)
                    return is_valid_memory_id(typed.memory_id);
                else
                    return is_valid_session_id(typed.session_id) &&
                           typed.through_turn > 0;
            },
            event.payload);
    if (!payload)
        return Result<void>::failure(
            {ErrorCode::InvalidInput, "memory event structure is invalid", false});
    return Result<void>::success();
}

} // namespace agent

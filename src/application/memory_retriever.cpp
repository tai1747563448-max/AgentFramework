#include "application/memory_retriever.h"
#include "domain/memory_event.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <tuple>
#include <utility>

namespace agent {
namespace {

bool decode_utf8(std::string_view text, std::vector<std::uint32_t>* points) {
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
        points->push_back(point);
        offset += width;
    }
    return true;
}

bool ascii_alphanumeric(std::uint32_t point) {
    return (point >= 'a' && point <= 'z') || (point >= 'A' && point <= 'Z') ||
           (point >= '0' && point <= '9');
}

std::uint32_t lowercase_ascii(std::uint32_t point) {
    return point >= 'A' && point <= 'Z' ? point - 'A' + 'a' : point;
}

bool cjk(std::uint32_t point) {
    return (point >= 0x3400 && point <= 0x4dbf) ||
           (point >= 0x4e00 && point <= 0x9fff) ||
           (point >= 0xf900 && point <= 0xfaff) ||
           (point >= 0x20000 && point <= 0x2ebef);
}

bool unicode_whitespace(std::uint32_t point) {
    return point == 0x0085 || point == 0x00a0 || point == 0x1680 ||
           (point >= 0x2000 && point <= 0x200a) || point == 0x2028 ||
           point == 0x2029 || point == 0x202f || point == 0x205f ||
           point == 0x3000;
}

bool unicode_punctuation(std::uint32_t point) {
    return (point >= 0x2000 && point <= 0x206f) ||
           (point >= 0x2e00 && point <= 0x2e7f) ||
           (point >= 0x3000 && point <= 0x303f) ||
           (point >= 0xfe10 && point <= 0xfe1f) ||
           (point >= 0xfe30 && point <= 0xfe4f) ||
           (point >= 0xfe50 && point <= 0xfe6f) ||
           (point >= 0xff01 && point <= 0xff0f) ||
           (point >= 0xff1a && point <= 0xff20) ||
           (point >= 0xff3b && point <= 0xff40) ||
           (point >= 0xff5b && point <= 0xff65);
}

std::set<std::u32string> tokens(const std::vector<std::uint32_t>& points) {
    std::set<std::u32string> result;
    std::u32string ascii_word;
    const auto flush_ascii = [&result, &ascii_word]() {
        if (!ascii_word.empty()) {
            result.insert(ascii_word);
            ascii_word.clear();
        }
    };
    for (std::size_t index = 0; index < points.size(); ++index) {
        const auto point = points[index];
        if (ascii_alphanumeric(point)) {
            ascii_word.push_back(static_cast<char32_t>(lowercase_ascii(point)));
            continue;
        }
        flush_ascii();
        if (point > 0x7f && !unicode_whitespace(point) &&
            !unicode_punctuation(point))
            result.insert(std::u32string(1, static_cast<char32_t>(point)));
        if (cjk(point) && index + 1 < points.size() && cjk(points[index + 1])) {
            result.insert(std::u32string{static_cast<char32_t>(point),
                                         static_cast<char32_t>(points[index + 1])});
        }
    }
    flush_ascii();
    return result;
}

std::string lowercase_ascii(std::string_view text) {
    std::string result(text);
    for (auto& character : result) {
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    }
    return result;
}

const char* category_name(MemoryCategory category) {
    switch (category) {
    case MemoryCategory::Preference:
        return "preference";
    case MemoryCategory::Decision:
        return "decision";
    case MemoryCategory::Fact:
        return "fact";
    case MemoryCategory::Workflow:
        return "workflow";
    case MemoryCategory::Constraint:
        return "constraint";
    }
    return "unknown";
}

Result<std::vector<std::string>> invalid_input() {
    return Result<std::vector<std::string>>::failure(
        {ErrorCode::InvalidInput, "memory retrieval input is invalid", false});
}

struct Candidate {
    const MemoryEntry* entry;
    std::size_t score;
    std::string normalized_updated_at_utc;
    std::string rendered;
};

} // namespace

Result<std::vector<std::string>> MemoryRetriever::retrieve(
    const MemoryState& state, std::string_view workspace_utf8, std::string_view query,
    std::size_t top_k, std::size_t injected_byte_budget) const {
    std::vector<std::uint32_t> workspace_points;
    std::vector<std::uint32_t> query_points;
    if (!decode_utf8(workspace_utf8, &workspace_points) ||
        !decode_utf8(query, &query_points))
        return invalid_input();
    const auto query_tokens = tokens(query_points);
    const std::string normalized_query = lowercase_ascii(query);
    std::vector<Candidate> candidates;
    for (const auto& pair : state.active_entries) {
        const auto& entry = pair.second;
        std::vector<std::uint32_t> content_points;
        std::vector<std::uint32_t> scope_points;
        std::vector<std::uint32_t> id_points;
        std::vector<std::uint32_t> updated_points;
        if (!decode_utf8(entry.content, &content_points) ||
            !decode_utf8(entry.scope_utf8, &scope_points) ||
            !decode_utf8(entry.memory_id, &id_points) ||
            !decode_utf8(entry.updated_at_utc, &updated_points))
            return invalid_input();
        if (!entry.scope_utf8.empty() && entry.scope_utf8 != workspace_utf8)
            continue;
        const auto entry_tokens = tokens(content_points);
        std::size_t overlap = 0;
        for (const auto& token : query_tokens) {
            if (entry_tokens.count(token) != 0)
                ++overlap;
        }
        if (overlap == 0)
            continue;
        const bool exact_phrase = !normalized_query.empty() &&
                                  lowercase_ascii(entry.content).find(normalized_query) !=
                                      std::string::npos;
        const auto score = overlap + (exact_phrase ? 100 : 0);
        candidates.push_back({&entry, score, normalized_memory_timestamp(entry.updated_at_utc),
                              "- [" + entry.memory_id + "] (" +
                                  category_name(entry.category) + ") " + entry.content});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left,
                                                        const Candidate& right) {
        if (left.score != right.score)
            return left.score > right.score;
        if (left.normalized_updated_at_utc != right.normalized_updated_at_utc)
            return left.normalized_updated_at_utc > right.normalized_updated_at_utc;
        return left.entry->memory_id < right.entry->memory_id;
    });
    std::vector<std::string> selected;
    std::size_t bytes = 0;
    for (const auto& candidate : candidates) {
        if (selected.size() == top_k)
            break;
        const std::size_t separator_bytes = selected.empty() ? 0 : 1;
        const auto remaining = injected_byte_budget - bytes;
        if (remaining < separator_bytes ||
            candidate.rendered.size() > remaining - separator_bytes)
            continue;
        bytes += separator_bytes + candidate.rendered.size();
        selected.push_back(candidate.rendered);
    }
    return Result<std::vector<std::string>>::success(std::move(selected));
}

} // namespace agent

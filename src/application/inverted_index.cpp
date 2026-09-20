#include "application/inverted_index.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agent {
namespace {

bool ascii_alnum(unsigned char c) {
    return std::isalnum(c) != 0;
}

std::string ascii_lower_token(std::string_view token) {
    std::string out(token);
    for (auto& ch : out) {
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    }
    return out;
}

bool is_cjk(unsigned char c) {
    return c >= 0x80;  // approximation: any non-ASCII byte in UTF-8
                       // starts a multi-byte sequence that contains CJK
                       // for our test corpora. Stage 7 can refine.
}

// Simple ASCII + per-CJK-character tokeniser. We don't decode the
// full UTF-8 codepoint stream; instead we treat each UTF-8 character
// as one token by detecting its leading byte and skipping the right
// number of continuation bytes (10xxxxxx). This keeps "用户" as
// two 3-byte tokens "用" and "户" rather than one 6-byte blob, so
// partial overlap (query "用" against content "用户") still hits.
std::vector<std::string> tokenize(std::string_view text) {
    std::vector<std::string> tokens;
    std::string current;
    const auto flush = [&]() {
        if (!current.empty()) {
            tokens.push_back(ascii_lower_token(current));
            current.clear();
        }
    };
    for (std::size_t i = 0; i < text.size();) {
        const auto u = static_cast<unsigned char>(text[i]);
        if (ascii_alnum(u) || u == '-' || u == '_') {
            current.push_back(static_cast<char>(u));
            ++i;
        } else if (u >= 0xc0 && u <= 0xef) {
            // UTF-8 2-byte (u >= 0xc0 && u <= 0xdf) or 3-byte
            // (u >= 0xe0 && u <= 0xef) leading byte.
            flush();
            std::size_t width = (u >= 0xe0) ? 3 : 2;
            if (i + width > text.size()) width = 1;
            tokens.emplace_back(text.substr(i, width));
            i += width;
        } else {
            flush();
            ++i;
        }
    }
    flush();
    return tokens;
}

}  // namespace

void InvertedIndex::clear() {
    token_to_ids_.clear();
    token_count_ = 0;
}

void InvertedIndex::add(const std::string& memory_id,
                        const std::string& content) {
    remove(memory_id);
    const auto tokens = tokenize(content);
    for (const auto& t : tokens) {
        // Skip single-byte tokens (noise). Multi-byte UTF-8 runs
        // (>= 2 bytes) are kept as-is.
        if (t.size() < 2) continue;
        token_to_ids_[t].push_back(memory_id);
        ++token_count_;
    }
}

void InvertedIndex::remove(const std::string& memory_id) {
    for (auto it = token_to_ids_.begin(); it != token_to_ids_.end();) {
        auto& ids = it->second;
        std::vector<std::string> kept;
        kept.reserve(ids.size());
        for (auto& id : ids) {
            if (id != memory_id) kept.push_back(std::move(id));
        }
        token_count_ -= (ids.size() - kept.size());
        if (kept.empty()) {
            it = token_to_ids_.erase(it);
        } else {
            ids = std::move(kept);
            ++it;
        }
    }
}

std::vector<std::string> InvertedIndex::candidates(
    std::string_view query) const {
    std::set<std::string> seen;
    std::vector<std::string> out;
    const auto tokens = tokenize(query);
    for (const auto& t : tokens) {
        if (t.size() < 2) continue;
        auto it = token_to_ids_.find(t);
        if (it == token_to_ids_.end()) continue;
        for (const auto& id : it->second) {
            if (seen.insert(id).second) out.push_back(id);
        }
    }
    return out;
}

}  // namespace agent

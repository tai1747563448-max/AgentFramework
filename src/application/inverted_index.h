#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace agent {

// Stage 6: inverted index over the memory entries. Tokenises content
// the same way MemoryRetriever does so the two stay aligned. Lookups
// are O(1) per token, O(k) for k tokens in the query.
class InvertedIndex {
public:
    void clear();
    std::size_t size() const noexcept { return token_count_; }

    // Add (or refresh) a memory entry's tokens into the index.
    void add(const std::string& memory_id, const std::string& content);

    // Remove a memory entry from the index entirely.
    void remove(const std::string& memory_id);

    // Return the candidate memory_ids that share at least one token
    // with the query. Order is not stable; callers must sort.
    std::vector<std::string> candidates(std::string_view query) const;

private:
    std::unordered_map<std::string, std::vector<std::string>> token_to_ids_;
    std::size_t token_count_{0};
};

}  // namespace agent

#include "adapters/workspace/workspace_text.h"

#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

namespace agent::workspace {
namespace {

constexpr std::array<std::uint32_t, 64> kSha256Constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

std::uint32_t rotate_right(std::uint32_t value, unsigned int count) {
    return (value >> count) | (value << (32U - count));
}

bool continuation(unsigned char byte) {
    return byte >= 0x80U && byte <= 0xBFU;
}

char ascii_fold(char character) {
    if (character >= 'A' && character <= 'Z') {
        return static_cast<char>(character + ('a' - 'A'));
    }
    return character;
}

Fault invalid_range() {
    return {FaultCode::InvalidArguments, "invalid line range", false};
}

}  // namespace

bool is_strict_utf8_text(std::string_view text) noexcept {
    for (std::size_t offset = 0; offset < text.size();) {
        const auto first = static_cast<unsigned char>(text[offset]);
        if (first == 0U) {
            return false;
        }
        if (first <= 0x7FU) {
            ++offset;
            continue;
        }
        if (first >= 0xC2U && first <= 0xDFU && offset + 1 < text.size() &&
            continuation(static_cast<unsigned char>(text[offset + 1]))) {
            offset += 2;
            continue;
        }
        if (first >= 0xE0U && first <= 0xEFU && offset + 2 < text.size()) {
            const auto second = static_cast<unsigned char>(text[offset + 1]);
            const auto third = static_cast<unsigned char>(text[offset + 2]);
            const bool second_valid =
                continuation(second) &&
                (first != 0xE0U || second >= 0xA0U) &&
                (first != 0xEDU || second <= 0x9FU);
            if (second_valid && continuation(third)) {
                offset += 3;
                continue;
            }
        }
        if (first >= 0xF0U && first <= 0xF4U && offset + 3 < text.size()) {
            const auto second = static_cast<unsigned char>(text[offset + 1]);
            const auto third = static_cast<unsigned char>(text[offset + 2]);
            const auto fourth = static_cast<unsigned char>(text[offset + 3]);
            const bool second_valid =
                continuation(second) &&
                (first != 0xF0U || second >= 0x90U) &&
                (first != 0xF4U || second <= 0x8FU);
            if (second_valid && continuation(third) && continuation(fourth)) {
                offset += 4;
                continue;
            }
        }
        return false;
    }
    return true;
}

std::string sha256_hex(std::string_view bytes) {
    std::vector<std::uint8_t> padded;
    padded.reserve(bytes.size() + 72);
    for (const char byte : bytes) {
        padded.push_back(static_cast<std::uint8_t>(
            static_cast<unsigned char>(byte)));
    }
    const auto bit_length = static_cast<std::uint64_t>(bytes.size()) * 8U;
    padded.push_back(0x80U);
    while ((padded.size() % 64U) != 56U) {
        padded.push_back(0U);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        padded.push_back(static_cast<std::uint8_t>(bit_length >> shift));
    }

    std::array<std::uint32_t, 8> hash{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    for (std::size_t chunk = 0; chunk < padded.size(); chunk += 64U) {
        std::array<std::uint32_t, 64> schedule{};
        for (std::size_t index = 0; index < 16U; ++index) {
            const auto offset = chunk + index * 4U;
            schedule[index] =
                (static_cast<std::uint32_t>(padded[offset]) << 24U) |
                (static_cast<std::uint32_t>(padded[offset + 1]) << 16U) |
                (static_cast<std::uint32_t>(padded[offset + 2]) << 8U) |
                static_cast<std::uint32_t>(padded[offset + 3]);
        }
        for (std::size_t index = 16U; index < schedule.size(); ++index) {
            const auto previous = schedule[index - 15U];
            const auto earlier = schedule[index - 2U];
            const auto sigma0 = rotate_right(previous, 7U) ^
                                rotate_right(previous, 18U) ^
                                (previous >> 3U);
            const auto sigma1 = rotate_right(earlier, 17U) ^
                                rotate_right(earlier, 19U) ^
                                (earlier >> 10U);
            schedule[index] = schedule[index - 16U] + sigma0 +
                              schedule[index - 7U] + sigma1;
        }

        auto a = hash[0];
        auto b = hash[1];
        auto c = hash[2];
        auto d = hash[3];
        auto e = hash[4];
        auto f = hash[5];
        auto g = hash[6];
        auto h = hash[7];
        for (std::size_t index = 0; index < schedule.size(); ++index) {
            const auto big_sigma1 = rotate_right(e, 6U) ^
                                    rotate_right(e, 11U) ^
                                    rotate_right(e, 25U);
            const auto choice = (e & f) ^ ((~e) & g);
            const auto temporary1 = h + big_sigma1 + choice +
                                    kSha256Constants[index] + schedule[index];
            const auto big_sigma0 = rotate_right(a, 2U) ^
                                    rotate_right(a, 13U) ^
                                    rotate_right(a, 22U);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temporary2 = big_sigma0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        hash[0] += a;
        hash[1] += b;
        hash[2] += c;
        hash[3] += d;
        hash[4] += e;
        hash[5] += f;
        hash[6] += g;
        hash[7] += h;
    }

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto word : hash) {
        output << std::setw(8) << word;
    }
    return output.str();
}

Outcome<LinePage> line_page(std::string_view text,
                            std::size_t start_line,
                            std::size_t max_lines,
                            std::size_t max_content_bytes) {
    if (start_line == 0 || max_lines == 0 || max_content_bytes == 0) {
        return invalid_range();
    }
    std::vector<std::pair<std::size_t, std::size_t>> lines;
    std::size_t begin = 0;
    for (std::size_t offset = 0; offset < text.size(); ++offset) {
        if (text[offset] == '\n') {
            lines.emplace_back(begin, offset + 1);
            begin = offset + 1;
        }
    }
    if (begin < text.size()) {
        lines.emplace_back(begin, text.size());
    }
    if (lines.empty()) {
        if (start_line != 1) {
            return invalid_range();
        }
        return LinePage{1, 0, 0, {}, std::nullopt};
    }
    if (start_line > lines.size()) {
        return invalid_range();
    }

    LinePage page;
    page.start_line = start_line;
    page.total_lines = lines.size();
    const auto first_index = start_line - 1;
    for (std::size_t index = first_index;
         index < lines.size() && index < first_index + max_lines; ++index) {
        const auto length = lines[index].second - lines[index].first;
        if (page.content.size() + length > max_content_bytes) {
            if (page.content.empty()) {
                return Fault{FaultCode::LimitExceeded,
                             "line exceeds workspace result limit", false};
            }
            break;
        }
        page.content.append(text.substr(lines[index].first, length));
        page.end_line = index + 1;
    }
    if (page.end_line < page.total_lines) {
        page.next_start_line = page.end_line + 1;
    }
    return page;
}

std::vector<std::size_t> literal_matches(std::string_view line,
                                         std::string_view query,
                                         bool case_sensitive) {
    std::vector<std::size_t> matches;
    if (query.empty() || query.size() > line.size()) {
        return matches;
    }
    for (std::size_t offset = 0; offset + query.size() <= line.size();) {
        bool equal = true;
        for (std::size_t index = 0; index < query.size(); ++index) {
            const auto left = case_sensitive ? line[offset + index]
                                             : ascii_fold(line[offset + index]);
            const auto right = case_sensitive ? query[index]
                                              : ascii_fold(query[index]);
            if (left != right) {
                equal = false;
                break;
            }
        }
        if (equal) {
            matches.push_back(offset);
            offset += query.size();
        } else {
            ++offset;
        }
    }
    return matches;
}

}  // namespace agent::workspace

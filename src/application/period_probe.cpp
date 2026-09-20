#include "application/period_probe.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace agent {
namespace {

bool decode_utf8(const std::string& text, std::vector<std::uint32_t>* points) {
    points->clear();
    for (std::size_t offset = 0; offset < text.size();) {
        const auto first = static_cast<unsigned char>(text[offset]);
        if (first == 0) return false;
        std::size_t width = 0;
        std::uint32_t point = 0;
        if (first <= 0x7f) {
            width = 1; point = first;
        } else if (first >= 0xc2 && first <= 0xdf) {
            width = 2; point = first & 0x1f;
        } else if (first >= 0xe0 && first <= 0xef) {
            width = 3; point = first & 0x0f;
        } else if (first >= 0xf0 && first <= 0xf4) {
            width = 4; point = first & 0x07;
        } else {
            return false;
        }
        if (offset + width > text.size()) return false;
        for (std::size_t i = 1; i < width; ++i) {
            const auto b = static_cast<unsigned char>(text[offset + i]);
            if ((b & 0xc0) != 0x80) return false;
            point = (point << 6) | (b & 0x3f);
        }
        if ((width == 3 && point < 0x800) || (width == 4 && point < 0x10000) ||
            (point >= 0xd800 && point <= 0xdfff) || point > 0x10ffff) {
            return false;
        }
        points->push_back(point);
        offset += width;
    }
    return true;
}

bool ascii_alnum(std::uint32_t p) {
    return (p >= 'a' && p <= 'z') || (p >= 'A' && p <= 'Z') || (p >= '0' && p <= '9');
}

std::uint32_t ascii_lower(std::uint32_t p) {
    return (p >= 'A' && p <= 'Z') ? p - 'A' + 'a' : p;
}

bool cjk(std::uint32_t p) {
    return (p >= 0x3400 && p <= 0x4dbf) ||
           (p >= 0x4e00 && p <= 0x9fff) ||
           (p >= 0xf900 && p <= 0xfaff) ||
           (p >= 0x20000 && p <= 0x2ebef);
}

bool unicode_ws(std::uint32_t p) {
    return p == 0x0085 || p == 0x00a0 || p == 0x1680 ||
           (p >= 0x2000 && p <= 0x200a) || p == 0x2028 ||
           p == 0x2029 || p == 0x202f || p == 0x205f || p == 0x3000;
}

bool unicode_punct(std::uint32_t p) {
    return (p >= 0x2000 && p <= 0x206f) ||
           (p >= 0x2e00 && p <= 0x2e7f) ||
           (p >= 0x3000 && p <= 0x303f) ||
           (p >= 0xfe10 && p <= 0xfe1f) ||
           (p >= 0xfe30 && p <= 0xfe4f) ||
           (p >= 0xfe50 && p <= 0xfe6f) ||
           (p >= 0xff01 && p <= 0xff0f) ||
           (p >= 0xff1a && p <= 0xff20) ||
           (p >= 0xff3b && p <= 0xff40) ||
           (p >= 0xff5b && p <= 0xff65);
}

std::set<std::u32string> tokens(const std::vector<std::uint32_t>& points) {
    std::set<std::u32string> result;
    std::u32string ascii_word;
    const auto flush = [&]() {
        if (!ascii_word.empty()) {
            result.insert(ascii_word);
            ascii_word.clear();
        }
    };
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto p = points[i];
        if (ascii_alnum(p)) {
            ascii_word.push_back(static_cast<char32_t>(ascii_lower(p)));
            continue;
        }
        flush();
        if (p > 0x7f && !unicode_ws(p) && !unicode_punct(p))
            result.insert(std::u32string(1, static_cast<char32_t>(p)));
        if (cjk(p) && i + 1 < points.size() && cjk(points[i + 1])) {
            result.insert(std::u32string{static_cast<char32_t>(p),
                                         static_cast<char32_t>(points[i + 1])});
        }
    }
    flush();
    return result;
}

double weighted_jaccard(const std::set<std::u32string>& a,
                        const std::set<std::u32string>& b) {
    if (a.empty() && b.empty()) return 0.0;
    if (a.empty() || b.empty()) return 0.0;
    std::size_t intersection = 0;
    const auto& smaller = a.size() <= b.size() ? a : b;
    const auto& larger  = a.size() <= b.size() ? b : a;
    for (const auto& t : smaller) {
        if (larger.count(t) != 0) ++intersection;
    }
    const auto uni = a.size() + b.size() - intersection;
    return uni == 0 ? 0.0 : static_cast<double>(intersection) / uni;
}

}  // namespace

PeriodProbe::AttachDecision PeriodProbe::decide(
    const std::vector<Period>& open_periods,
    const std::string& current_prompt,
    const PeriodConfig& config) const {
    AttachDecision out;
    if (open_periods.empty()) {
        out.reason = "no open periods";
        return out;
    }

    std::vector<std::uint32_t> prompt_points;
    if (!decode_utf8(current_prompt, &prompt_points)) {
        out.reason = "prompt is not valid UTF-8";
        return out;
    }
    const auto prompt_tokens = tokens(prompt_points);
    if (prompt_tokens.empty()) {
        out.reason = "prompt has no usable tokens";
        return out;
    }

    double best = 0.0;
    const Period* best_period = nullptr;
    for (const auto& p : open_periods) {
        std::vector<std::uint32_t> summary_points;
        if (!decode_utf8(p.episodic_summary, &summary_points)) continue;
        const auto summary_tokens = tokens(summary_points);
        const double sim = weighted_jaccard(prompt_tokens, summary_tokens);
        if (sim > best) {
            best = sim;
            best_period = &p;
        }
    }

    out.similarity = best;
    if (best_period != nullptr && best >= config.auto_attach_threshold) {
        out.attach = true;
        out.period_id = best_period->period_id;
        out.reason = "similarity " + std::to_string(best) +
                     " >= threshold " +
                     std::to_string(config.auto_attach_threshold);
    } else {
        out.reason = "best similarity " + std::to_string(best) +
                     " < threshold " +
                     std::to_string(config.auto_attach_threshold);
    }
    return out;
}

}  // namespace agent

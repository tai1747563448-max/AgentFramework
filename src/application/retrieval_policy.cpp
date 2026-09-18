#include "application/retrieval_policy.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace agent {
namespace {

// T5: a tiny UTF-8 decoder that only needs to locate ASCII bytes and
// the CJK Unified Ideographs range. Anything else (Latin, Cyrillic,
// emoji, ...) stays inert for the keyword scan.
bool decode_codepoint(std::string_view text, std::size_t& offset,
                      std::uint32_t& point) noexcept {
    if (offset >= text.size()) return false;
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first <= 0x7FU) {
        point = first;
        offset += 1;
        return true;
    }
    std::size_t width = 0;
    std::uint32_t value = 0;
    if ((first & 0xE0U) == 0xC0U) { width = 2; value = first & 0x1FU; }
    else if ((first & 0xF0U) == 0xE0U) { width = 3; value = first & 0x0FU; }
    else if ((first & 0xF8U) == 0xF0U) { width = 4; value = first & 0x07U; }
    else return false;
    if (offset + width > text.size()) return false;
    for (std::size_t index = 1; index < width; ++index) {
        const auto byte = static_cast<unsigned char>(text[offset + index]);
        if ((byte & 0xC0U) != 0x80U) return false;
        value = (value << 6) | (byte & 0x3FU);
    }
    point = value;
    offset += width;
    return true;
}

bool is_cjk(std::uint32_t point) noexcept {
    return (point >= 0x3400U && point <= 0x4DBFU) ||
           (point >= 0x4E00U && point <= 0x9FFFU) ||
           (point >= 0xF900U && point <= 0xFAFFU) ||
           (point >= 0x20000U && point <= 0x323AFU);
}

bool contains_cjk(std::string_view text) noexcept {
    std::size_t offset = 0;
    std::uint32_t point = 0;
    while (decode_codepoint(text, offset, point)) {
        if (is_cjk(point)) return true;
    }
    return false;
}

std::string lower_ascii(std::string_view text) {
    std::string lower(text);
    for (auto& byte : lower) {
        if (byte >= 'A' && byte <= 'Z')
            byte = static_cast<char>(byte - 'A' + 'a');
    }
    return lower;
}

bool ascii_letter(char byte) noexcept {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
}

// T5: returns true when `needle` is found in `haystack` at a position
// that is not surrounded by ASCII letters or digits. This avoids
// false positives from substrings (e.g. "rule" inside "ruler").
bool contains_word(const std::string& haystack, std::string_view needle) {
    if (needle.empty()) return false;
    std::size_t position = 0;
    while ((position = haystack.find(needle.data(), position,
                                     needle.size())) != std::string::npos) {
        const auto end = position + needle.size();
        const bool left_ok = position == 0 ||
                             !ascii_letter(haystack[position - 1]) &&
                                 !std::isdigit(static_cast<unsigned char>(
                                     haystack[position - 1]));
        const bool right_ok = end == haystack.size() ||
                              !ascii_letter(haystack[end]) &&
                                  !std::isdigit(static_cast<unsigned char>(
                                      haystack[end]));
        if (left_ok && right_ok) return true;
        position = end;
    }
    return false;
}

// T5: a stable set of "the user is asking a regulatory question"
// signals. Omission phrasing, follow-up "what about the exception",
// citation, regulatory code mixed with a programming question, and
// direct regulatory vocabulary must trigger retrieval. Any ordinary
// task that lacks these markers is allowed to skip.

constexpr const char* kEnglishRegulatoryWords[] = {
    "regulation", "regulations", "rule", "rules",
    "cfr", "ecfr", "federal register",
    "statute", "statutes", "law", "legal",
    "compliance", "requirement", "requirements",
    "exemption", "exception", "permit",
    "citation", "cite", "reference", "section",
    "policy", "policies", "guideline", "guidelines"};

constexpr const char* kEnglishFollowUpPhrases[] = {
    "what about the exception",
    "what about exceptions",
    "what about the exemption",
    "what about the rule",
    "what about the regulation",
    "and the exception",
    "any exceptions",
    "any exemption"};

// T5: regulatory-style code-with-context phrasing seen in real CLI
// prompts. The plan mandates retrieval for "regulatory code mixed
// with programming questions". We only require retrieval when at
// least one CJK or English regulatory marker and at least one
// programming artifact (file extension, build tool, or canonical
// keyword) co-occur.
constexpr const char* kProgrammingArtifacts[] = {
    ".py", ".cpp", ".cc", ".cxx", ".h", ".hpp", ".go", ".rs", ".java",
    ".ts", ".tsx", ".js", ".jsx",
    "cmake", "makefile", "conanfile", "toml", "yaml", "yml", "json",
    "cargo", "mvn ", "gradle", "pip ", "pytest"};

// T5: stable, loggable reason codes.
constexpr const char* kReasonUserOptedOut = "user_opted_out";
constexpr const char* kReasonExplicitAlways = "user_explicit_always";
constexpr const char* kReasonSessionCarry = "session_always_required";
constexpr const char* kReasonCitation = "regulatory_citation";
constexpr const char* kReasonOmission = "regulatory_omission";
constexpr const char* kReasonFollowUp = "regulatory_followup";
constexpr const char* kReasonCodeWithRegulation =
    "regulatory_code_mixed_question";
constexpr const char* kReasonOrdinaryEnglish = "no_regulatory_signal_en";
constexpr const char* kReasonOrdinaryCjk = "no_regulatory_signal_cjk";

bool has_citation_marker(const std::string& lower) {
    // "40 CFR 60.1", "40 CFR § 60.1", "21 U.S.C. 343", etc. We only
    // scan ASCII fragments; CJK citation forms trigger via the
    // regulatory vocabulary below.
    std::size_t offset = 0;
    while (offset < lower.size()) {
        const auto cfr = lower.find("cfr", offset);
        if (cfr == std::string::npos) break;
        const auto left = cfr == 0 ? ' ' : lower[cfr - 1];
        const auto right = cfr + 3 >= lower.size() ? ' ' : lower[cfr + 3];
        const bool left_ok = !std::isdigit(static_cast<unsigned char>(left));
        const bool right_ok = !std::isdigit(static_cast<unsigned char>(right));
        if (left_ok && right_ok) return true;
        offset = cfr + 3;
    }
    return contains_word(lower, "usc") ||
           contains_word(lower, "u.s.c") ||
           contains_word(lower, "section");
}

bool has_omission_marker(const std::string& lower, bool cjk) {
    if (cjk) {
        for (const char* phrase : {
            u8"省略",
            u8"省略掉",
            u8"忽略",
            u8"不算",
            u8"不必引用",
            u8"不用引用",
            u8"不要引用",
            u8"不必查",
            u8"不用查",
            u8"不用检索",
            u8"不需要引文",
            u8"不要引文",
            u8"不要出处",
            u8"不需要出处"}) {
            if (lower.find(phrase) != std::string::npos) return true;
        }
        return false;
    }
    for (const char* phrase : {
        "skip citation", "skip citations", "no citation needed",
        "no citations needed", "no need to cite",
        "without citation", "without citations",
        "without citing", "don't bother citing",
        "no reference needed", "skip the reference",
        "no official source"}) {
        if (lower.find(phrase) != std::string::npos) return true;
    }
    return false;
}

bool has_followup_marker(const std::string& lower, bool cjk) {
    if (cjk) {
        for (const char* phrase : {
            u8"那例外呢",
            u8"那例外的呢",
            u8"那豁免呢",
            u8"那豁免情况呢",
            u8"那规则呢",
            u8"那规章呢",
            u8"那这条呢",
            u8"还有例外吗",
            u8"有例外吗",
            u8"有豁免吗",
            u8"那怎么办",
            u8"那怎么处理",
            u8"这种情况呢",
            u8"那种情况呢",
            u8"那如果",
            u8"如果是这种情况",
            u8"如果我"}) {
            if (lower.find(phrase) != std::string::npos) return true;
        }
        return false;
    }
    for (const char* phrase : kEnglishFollowUpPhrases) {
        if (lower.find(phrase) != std::string::npos) return true;
    }
    return false;
}

bool has_regulatory_vocabulary(const std::string& lower, bool cjk) {
    if (cjk) {
        for (const char* word : {
            u8"法规",
            u8"规章",
            u8"规则",
            u8"条例",
            u8"法律",
            u8"法定",
            u8"合规",
            u8"遵约",
            u8"要求",
            u8"条款",
            u8"条文",
            u8"豁免",
            u8"例外",
            u8"许可",
            u8"许可证",
            u8"引用",
            u8"出处",
            u8"依据",
            u8"管辖",
            u8"标准",
            u8"排放",
            u8"污染物",
            u8"危险品",
            u8"化学品",
            u8"安全",
            u8"监管"}) {
            if (lower.find(word) != std::string::npos) return true;
        }
        return false;
    }
    for (const char* word : kEnglishRegulatoryWords) {
        if (contains_word(lower, word)) return true;
    }
    return false;
}

bool has_programming_artifact(const std::string& lower) {
    for (const char* artifact : kProgrammingArtifacts) {
        if (lower.find(artifact) != std::string::npos) return true;
    }
    return contains_word(lower, "function") ||
           contains_word(lower, "class") ||
           contains_word(lower, "method") ||
           contains_word(lower, "compile") ||
           contains_word(lower, "build") ||
           contains_word(lower, "test") ||
           contains_word(lower, "debug");
}

}  // namespace

RetrievalDecision decide_retrieval(std::string_view text,
                                   RetrievalPolicy explicit_policy,
                                   bool session_regulatory_context) {
    // T5: explicit Off MUST be honoured even if the prompt contains
    // a regulatory keyword. The CLI only ever sets `Off` in response
    // to an unambiguous user choice ("/rag off", CLI flag, or an
    // opt-out phrasing handled by the persistent provider).
    if (explicit_policy == RetrievalPolicy::Off) {
        return {RetrievalNeed::None, kReasonUserOptedOut};
    }

    const auto cjk = contains_cjk(text);
    const auto lower = lower_ascii(text);
    const bool regulatory_signal =
        has_citation_marker(lower) ||
        has_omission_marker(lower, cjk) ||
        has_followup_marker(lower, cjk) ||
        has_regulatory_vocabulary(lower, cjk);

    // T5: explicit Always overrides the heuristic. When the policy
    // requires retrieval we still distinguish between ExactReference
    // (a citation was supplied, so we must hit the deterministic
    // index lookup) and Semantic (no citation, but the caller asked
    // anyway).
    if (explicit_policy == RetrievalPolicy::Always) {
        return {has_citation_marker(lower) ? RetrievalNeed::ExactReference
                                           : RetrievalNeed::Semantic,
                kReasonExplicitAlways};
    }

    // T5: when the prompt lacks a regulatory marker but the session
    // previously needed retrieval, keep it on. The carry flag never
    // skips on its own. We do not "upgrade" Semantic to Exact — that
    // decision still belongs to the heuristic.
    if (!regulatory_signal && session_regulatory_context) {
        return {RetrievalNeed::Semantic, kReasonSessionCarry};
    }

    if (regulatory_signal) {
        if (has_citation_marker(lower)) {
            return {RetrievalNeed::ExactReference, kReasonCitation};
        }
        if (has_followup_marker(lower, cjk)) {
            return {RetrievalNeed::Semantic, kReasonFollowUp};
        }
        if (has_omission_marker(lower, cjk)) {
            // T5: an "omit citation" instruction does not bypass
            // retrieval; we still have to look the article up so the
            // user can confirm which citation was intentionally
            // skipped.
            return {RetrievalNeed::Semantic, kReasonOmission};
        }
        if (has_regulatory_vocabulary(lower, cjk) &&
            has_programming_artifact(lower)) {
            return {RetrievalNeed::Semantic, kReasonCodeWithRegulation};
        }
        return {RetrievalNeed::Semantic,
                cjk ? kReasonOrdinaryCjk : kReasonOrdinaryEnglish};
    }

    return {RetrievalNeed::None,
            cjk ? kReasonOrdinaryCjk : kReasonOrdinaryEnglish};
}

}  // namespace agent
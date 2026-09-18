#pragma once

#include <string>
#include <string_view>

namespace agent {

// T5: per-turn retrieval policy.
//
// The CLI and session layer pass a single explicit policy into
// `decide_retrieval`. The function is local, conservative, and never
// performs a network call. The session regulatory context flag is only
// consulted as a "keep retrieval on" hint; it cannot turn retrieval
// off when the user prompt has no regulatory keyword.
enum class RetrievalPolicy {
    Auto,
    Always,
    Off,
};

// T5: what the runtime should prepare on the RAG side for a turn.
//
// `None` means skip retrieval entirely. `ExactReference` means
// a deterministic citation is required (eCFR-style lookup). `Semantic`
// means a dense embedding is required (topical evidence).
enum class RetrievalNeed {
    None,
    ExactReference,
    Semantic,
};

struct RetrievalDecision {
    RetrievalNeed need{RetrievalNeed::None};
    std::string reason_code;
};

// T5: route a user prompt to a retrieval decision.
//
// Precedence:
//   * `Off`  : the user (or programmatic caller) explicitly opted out
//              — `RetrievalNeed::None` is the only valid answer.
//   * `Always`: the caller explicitly asked for retrieval — the
//              heuristic is skipped and the policy decides between
//              `ExactReference` and `Semantic` from the prompt only.
//   * `Auto` : the heuristic below decides.
//
// `session_regulatory_context` may only preserve a previously-needed
// retrieval. It never causes a skip on its own when the prompt has no
// regulatory keyword.
RetrievalDecision decide_retrieval(std::string_view text,
                                   RetrievalPolicy explicit_policy,
                                   bool session_regulatory_context);

}  // namespace agent
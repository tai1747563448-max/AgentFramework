#include "application/retrieval_policy.h"
#include "test_support.h"

#include <string>
#include <vector>

namespace {

void assert_decision(const agent::RetrievalDecision& decision,
                    agent::RetrievalNeed expected_need,
                    const char* expected_reason) {
    REQUIRE(decision.need == expected_need);
    REQUIRE(decision.reason_code == expected_reason);
}

}  // namespace

TEST_CASE(retrieval_policy_off_overrides_regulatory_signals) {
    // T5: Off is only ever set by an explicit user choice. Even a
    // perfect regulatory prompt must skip retrieval.
    const std::vector<std::string> prompts{
        "What does 40 CFR 60.1 say about emissions?",
        u8"请帮我查找40 CFR § 60.1 条款",
        u8"那例外呢？",
        "Skip citations and just answer in your own words."};
    for (const auto& prompt : prompts) {
        const auto decision = agent::decide_retrieval(
            prompt, agent::RetrievalPolicy::Off, false);
        assert_decision(decision, agent::RetrievalNeed::None,
                        "user_opted_out");
        const auto carried = agent::decide_retrieval(
            prompt, agent::RetrievalPolicy::Off, true);
        assert_decision(carried, agent::RetrievalNeed::None,
                        "user_opted_out");
    }
}

TEST_CASE(retrieval_policy_always_overrides_heuristic_and_picks_exact_for_citations) {
    const auto with_citation = agent::decide_retrieval(
        "Cite 40 CFR 60.1 verbatim", agent::RetrievalPolicy::Always, false);
    assert_decision(with_citation, agent::RetrievalNeed::ExactReference,
                    "user_explicit_always");

    const auto without_citation = agent::decide_retrieval(
        "Explain the rule", agent::RetrievalPolicy::Always, false);
    assert_decision(without_citation, agent::RetrievalNeed::Semantic,
                    "user_explicit_always");

    // T5: Always wins even when the prompt is a perfectly ordinary
    // chat question.
    const auto chatty = agent::decide_retrieval(
        "Hi, how are you?", agent::RetrievalPolicy::Always, false);
    assert_decision(chatty, agent::RetrievalNeed::Semantic,
                    "user_explicit_always");
}

TEST_CASE(retrieval_policy_auto_routes_citations_to_exact_reference) {
    const auto english = agent::decide_retrieval(
        "What does 40 CFR 60.1 say about emissions?", agent::RetrievalPolicy::Auto, false);
    assert_decision(english, agent::RetrievalNeed::ExactReference,
                    "regulatory_citation");

    const auto english_section = agent::decide_retrieval(
        "Quote 21 U.S.C. 343(i)(2).", agent::RetrievalPolicy::Auto, false);
    assert_decision(english_section, agent::RetrievalNeed::ExactReference,
                    "regulatory_citation");

    const auto cjk = agent::decide_retrieval(
        u8"请引用 40 CFR § 60.1 给我看一下原文。",
        agent::RetrievalPolicy::Auto, false);
    assert_decision(cjk, agent::RetrievalNeed::ExactReference,
                    "regulatory_citation");
}

TEST_CASE(retrieval_policy_auto_triggers_on_followup_phrasing) {
    const std::vector<std::string> followups{
        u8"那例外呢？",
        u8"那例外的呢？",
        u8"那豁免情况呢？",
        "What about the exception?",
        "What about the exemption?",
        "And the exception in 60.1?"};
    for (const auto& prompt : followups) {
        const auto decision = agent::decide_retrieval(
            prompt, agent::RetrievalPolicy::Auto, false);
        assert_decision(decision, agent::RetrievalNeed::Semantic,
                        "regulatory_followup");
    }
}

TEST_CASE(retrieval_policy_auto_still_runs_omission_requests) {
    // T5: "skip the citation" must NOT bypass retrieval. The
    // orchestrator still needs to know which article would have been
    // cited so the user can audit the choice.
    const auto english = agent::decide_retrieval(
        "Answer without citing anything about the emission limits.",
        agent::RetrievalPolicy::Auto, false);
    assert_decision(english, agent::RetrievalNeed::Semantic,
                    "regulatory_omission");

    const auto cjk = agent::decide_retrieval(
        u8"不要引用，给我讲讲合规要求。",
        agent::RetrievalPolicy::Auto, false);
    assert_decision(cjk, agent::RetrievalNeed::Semantic,
                    "regulatory_omission");
}

TEST_CASE(retrieval_policy_auto_triggers_on_regulatory_vocabulary_alone) {
    const std::vector<std::string> prompts{
        "Does the regulation require a permit for that emission source?",
        "Tell me about the compliance requirements for hazardous waste.",
        u8"请告诉我这条法规的适用范围。",
        u8"这个豁免条款具体怎么说？",
        u8"排放标准对小型锅炉的要求是什么？"};
    for (const auto& prompt : prompts) {
        const auto decision = agent::decide_retrieval(
            prompt, agent::RetrievalPolicy::Auto, false);
        REQUIRE(decision.need != agent::RetrievalNeed::None);
        REQUIRE(!decision.reason_code.empty());
    }
}

TEST_CASE(retrieval_policy_auto_requires_retrieval_for_code_with_regulatory_context) {
    const auto english = agent::decide_retrieval(
        "Implement a Python validator for 40 CFR 60.1 emissions.",
        agent::RetrievalPolicy::Auto, false);
    REQUIRE(english.need != agent::RetrievalNeed::None);

    const auto cjk = agent::decide_retrieval(
        u8"在 CMakeLists.txt 里加上对 40 CFR 60 排放合规的检查。",
        agent::RetrievalPolicy::Auto, false);
    REQUIRE(cjk.need != agent::RetrievalNeed::None);
}

TEST_CASE(retrieval_policy_auto_allows_ordinary_chat_to_skip) {
    const std::vector<std::string> prompts{
        "Hello, how are you?",
        "Could you refactor src/foo.cpp to use shared_ptr?",
        "What is the capital of France?",
        u8"帮我重写一下 main.cpp 的循环",
        u8"这个 yaml 怎么解析？"};
    for (const auto& prompt : prompts) {
        const auto decision = agent::decide_retrieval(
            prompt, agent::RetrievalPolicy::Auto, false);
        assert_decision(decision, agent::RetrievalNeed::None,
                        decision.reason_code.c_str());
        REQUIRE(decision.need == agent::RetrievalNeed::None);
    }
}

TEST_CASE(retrieval_policy_session_carry_only_preserves_a_prior_need) {
    // T5: a session regulatory flag MUST NOT skip retrieval on its
    // own when the prompt lacks a regulatory keyword. It only
    // confirms that a turn that previously needed retrieval still
    // needs it.
    const auto prompt = std::string("Please continue editing the helper.");
    const auto no_carry = agent::decide_retrieval(
        prompt, agent::RetrievalPolicy::Auto, false);
    assert_decision(no_carry, agent::RetrievalNeed::None,
                    no_carry.reason_code.c_str());
    REQUIRE(no_carry.need == agent::RetrievalNeed::None);

    const auto carry = agent::decide_retrieval(
        prompt, agent::RetrievalPolicy::Auto, true);
    assert_decision(carry, agent::RetrievalNeed::Semantic,
                    "session_always_required");

    // T5: the carry flag cannot downgrade ExactReference either. A
    // follow-up with a citation must still go through the exact
    // path.
    const auto exact_followup = agent::decide_retrieval(
        "And 40 CFR 60.1 specifically?", agent::RetrievalPolicy::Auto, true);
    assert_decision(exact_followup, agent::RetrievalNeed::ExactReference,
                    "regulatory_citation");
}

TEST_CASE(retrieval_policy_reason_codes_are_stable_for_logging) {
    // T5: the orchestrator matches on the reason code, so the codes
    // must not change accidentally.
    REQUIRE(std::string("user_opted_out") ==
            agent::decide_retrieval("hi",
                                    agent::RetrievalPolicy::Off,
                                    false).reason_code);
    REQUIRE(std::string("user_explicit_always") ==
            agent::decide_retrieval("hi",
                                    agent::RetrievalPolicy::Always,
                                    false).reason_code);
    REQUIRE(std::string("session_always_required") ==
            agent::decide_retrieval("hi",
                                    agent::RetrievalPolicy::Auto,
                                    true).reason_code);
    REQUIRE(std::string("regulatory_citation") ==
            agent::decide_retrieval("40 CFR 60.1",
                                    agent::RetrievalPolicy::Auto,
                                    false).reason_code);
    REQUIRE(std::string("regulatory_followup") ==
            agent::decide_retrieval(u8"那例外呢",
                                    agent::RetrievalPolicy::Auto,
                                    false).reason_code);
}

TEST_CASE(retrieval_policy_handles_empty_input_without_crashing) {
    const auto empty = agent::decide_retrieval(
        "", agent::RetrievalPolicy::Auto, false);
    REQUIRE(empty.need == agent::RetrievalNeed::None);

    const auto whitespace = agent::decide_retrieval(
        "   \t\n", agent::RetrievalPolicy::Auto, false);
    REQUIRE(whitespace.need == agent::RetrievalNeed::None);

    const auto off = agent::decide_retrieval(
        "", agent::RetrievalPolicy::Off, true);
    REQUIRE(off.need == agent::RetrievalNeed::None);
    REQUIRE(off.reason_code == std::string("user_opted_out"));
}
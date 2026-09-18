#include "application/runtime_engine.h"
#include "test_support.h"

#include <string>

namespace {

using agent::ContinueReason;
using agent::continue_reason_name;

TEST_CASE(continue_reason_name_returns_stable_strings) {
    REQUIRE(std::string(continue_reason_name(
                ContinueReason::InitialCreate)) == "initial_create");
    REQUIRE(std::string(continue_reason_name(
                ContinueReason::ContextPrepared)) == "context_prepared");
    REQUIRE(std::string(continue_reason_name(
                ContinueReason::ContextFailed)) == "context_failed");
    REQUIRE(std::string(continue_reason_name(
                ContinueReason::KnowledgeNoMatch)) == "knowledge_no_match");
    REQUIRE(std::string(continue_reason_name(
                ContinueReason::AwaitingModelNextRound)) ==
            "awaiting_model_next");
    REQUIRE(std::string(continue_reason_name(
                ContinueReason::AwaitingToolNext)) ==
            "awaiting_tool_next");
    REQUIRE(std::string(continue_reason_name(
                ContinueReason::ToolsCompletedRound)) ==
            "tools_completed");
    REQUIRE(std::string(continue_reason_name(
                ContinueReason::ModelResponseAccepted)) ==
            "model_response_accepted");
    REQUIRE(std::string(continue_reason_name(
                ContinueReason::CompactionSucceeded)) ==
            "compaction_succeeded");
    REQUIRE(std::string(continue_reason_name(
                ContinueReason::CancelledByUser)) ==
            "cancelled_by_user");
    REQUIRE(std::string(continue_reason_name(
                ContinueReason::BudgetExceeded)) ==
            "budget_exceeded");
    REQUIRE(std::string(continue_reason_name(
                ContinueReason::InvariantFailure)) ==
            "invariant_failure");
}

TEST_CASE(continue_reason_names_are_unique) {
    const agent::ContinueReason reasons[] = {
        ContinueReason::InitialCreate,
        ContinueReason::ContextPrepared,
        ContinueReason::ContextFailed,
        ContinueReason::KnowledgeNoMatch,
        ContinueReason::AwaitingModelNextRound,
        ContinueReason::AwaitingToolNext,
        ContinueReason::ToolsCompletedRound,
        ContinueReason::ModelResponseAccepted,
        ContinueReason::CompactionSucceeded,
        ContinueReason::CancelledByUser,
        ContinueReason::BudgetExceeded,
        ContinueReason::InvariantFailure,
    };
    for (std::size_t i = 0; i < std::size(reasons); ++i) {
        for (std::size_t j = i + 1; j < std::size(reasons); ++j) {
            REQUIRE(std::string(continue_reason_name(reasons[i])) !=
                    std::string(continue_reason_name(reasons[j])));
        }
    }
}

}  // namespace
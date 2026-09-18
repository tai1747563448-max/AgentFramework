#include "domain/task_state.h"
#include "test_support.h"

#include <string>

using agent::parse_task_type;
using agent::TaskType;
using agent::task_type_name;

TEST_CASE(task_type_name_is_stable_for_every_value) {
    REQUIRE(std::string(task_type_name(TaskType::LocalBash)) ==
            "local_bash");
    REQUIRE(std::string(task_type_name(TaskType::LocalAgent)) ==
            "local_agent");
    REQUIRE(std::string(task_type_name(TaskType::InProcessTeammate)) ==
            "in_process_teammate");
}

TEST_CASE(parse_task_type_round_trips_through_namer) {
    REQUIRE(parse_task_type("local_bash") == TaskType::LocalBash);
    REQUIRE(parse_task_type("local_agent") == TaskType::LocalAgent);
    REQUIRE(parse_task_type("in_process_teammate") ==
            TaskType::InProcessTeammate);
}

TEST_CASE(parse_task_type_unknown_falls_back_to_local_bash) {
    // Unknown task type values from the model resolve to a safe
    // host-runnable default so a typo in the response does not
    // crash the dispatch path.
    REQUIRE(parse_task_type("cloudflare_worker") == TaskType::LocalBash);
    REQUIRE(parse_task_type("") == TaskType::LocalBash);
}

TEST_CASE(task_state_background_field_defaults_to_false) {
    agent::TaskState state;
    REQUIRE(!state.background);
}

TEST_CASE(task_state_equality_includes_background) {
    agent::TaskState left;
    agent::TaskState right;
    REQUIRE(left == right);
    right.background = true;
    REQUIRE(!(left == right));
}

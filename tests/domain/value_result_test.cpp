#include "domain/result.h"
#include "domain/value.h"
#include "test_support.h"

TEST_CASE(value_preserves_recursive_structure) {
    agent::Value value = agent::Value::object({
        {"command", agent::Value("cmake --build build")},
        {"flags", agent::Value::array({agent::Value("Debug"), agent::Value(true)})}
    });
    REQUIRE(value.at("command").as_string() == "cmake --build build");
    REQUIRE(value.at("flags").as_array().at(1).as_bool());
}

TEST_CASE(result_separates_value_from_error) {
    auto ok = agent::Result<int>::success(7);
    auto bad = agent::Result<int>::failure(
        {agent::ErrorCode::InvalidInput, "missing issue", false});
    REQUIRE(ok.has_value());
    REQUIRE(ok.value() == 7);
    REQUIRE(!bad.has_value());
    REQUIRE(bad.error().code == agent::ErrorCode::InvalidInput);
}

TEST_CASE(void_result_separates_success_from_error) {
    auto ok = agent::Result<void>::success();
    auto bad = agent::Result<void>::failure(
        {agent::ErrorCode::Cancelled, "cancelled by user", false});
    ok.value();
    REQUIRE(ok.has_value());
    REQUIRE(!bad.has_value());
    REQUIRE(bad.error().code == agent::ErrorCode::Cancelled);
}

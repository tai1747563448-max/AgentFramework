#include "adapters/json/strict_json.h"

#include "test_support.h"

#include <string>

using agent::StrictJsonError;
using agent::parse_strict_json;

namespace {

std::string repeat(const std::string& value, std::size_t count) {
    std::string out;
    out.reserve(value.size() * count);
    for (std::size_t i = 0; i < count; ++i) out += value;
    return out;
}

}  // namespace

TEST_CASE(StrictJsonRejectsTopLevelDuplicateKey) {
    const auto result = parse_strict_json(R"({"a":1,"a":2})", 1024);
    REQUIRE(!result.value.has_value());
    REQUIRE(result.error.has_value());
    REQUIRE(result.error->kind == StrictJsonError::Kind::DuplicateKey);
}

TEST_CASE(StrictJsonRejectsNestedDuplicateKey) {
    const auto result = parse_strict_json(
        R"({"outer":{"x":1,"x":2}})", 1024);
    REQUIRE(!result.value.has_value());
    REQUIRE(result.error.has_value());
    REQUIRE(result.error->kind == StrictJsonError::Kind::DuplicateKey);
}

TEST_CASE(StrictJsonAllowsSameKeyInDifferentObjects) {
    const auto result = parse_strict_json(
        R"({"a":{"x":1},"b":{"x":2}})", 1024);
    REQUIRE(result.value.has_value());
    REQUIRE(result.value->at("a").at("x").get<int>() == 1);
    REQUIRE(result.value->at("b").at("x").get<int>() == 2);
}

TEST_CASE(StrictJsonRejectsEmptyInput) {
    const auto result = parse_strict_json("", 1024);
    REQUIRE(!result.value.has_value());
    REQUIRE(result.error.has_value());
    REQUIRE(result.error->kind == StrictJsonError::Kind::Empty);
}

TEST_CASE(StrictJsonRejectsTrailingContent) {
    const auto result = parse_strict_json(R"({"a":1} garbage)", 1024);
    REQUIRE(!result.value.has_value());
    REQUIRE(result.error.has_value());
}

TEST_CASE(StrictJsonRejectsTruncatedInput) {
    const auto result = parse_strict_json(R"({"a":)", 1024);
    REQUIRE(!result.value.has_value());
    REQUIRE(result.error.has_value());
}

TEST_CASE(StrictJsonRejectsInvalidNumber) {
    const auto result = parse_strict_json(R"({"a":01})", 1024);
    REQUIRE(!result.value.has_value());
    REQUIRE(result.error.has_value());
}

TEST_CASE(StrictJsonRejectsDepthOverflow) {
    std::string nested = "[";
    nested += repeat("[", agent::kStrictJsonMaxDepth + 4);
    nested += repeat("]", agent::kStrictJsonMaxDepth + 4);
    const auto result = parse_strict_json(nested, 1024 * 1024);
    REQUIRE(!result.value.has_value());
    REQUIRE(result.error.has_value());
    REQUIRE(result.error->kind == StrictJsonError::Kind::DepthExceeded);
}

TEST_CASE(StrictJsonRejectsOversizedInput) {
    const std::string body(2048, ' ');
    const auto result = parse_strict_json(body, 1024);
    REQUIRE(!result.value.has_value());
    REQUIRE(result.error.has_value());
    REQUIRE(result.error->kind == StrictJsonError::Kind::TooLarge);
}

TEST_CASE(StrictJsonLargeArrayLinearTime) {
    // Reproduce the original 64k-entry manifest shape. We do not assert on
    // wall-clock time here; the dedicated benchmark in benchmarks/results
    // covers the regression budget.
    std::string body = "[";
    for (int i = 0; i < 64000; ++i) {
        if (i) body += ",";
        body += "{\"k\":\"" + std::to_string(i) + "\",\"v\":" +
                std::to_string(i) + "}";
    }
    body += "]";
    const auto result = parse_strict_json(body, 64 * 1024 * 1024);
    REQUIRE(result.value.has_value());
    REQUIRE(result.value->is_array());
    REQUIRE(result.value->size() == 64000u);
}

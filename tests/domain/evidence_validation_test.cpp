#include "domain/evidence_validation.h"
#include "domain/model_types.h"
#include "test_support.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace fixtures {

agent::Evidence item(std::string source_id = "guide.md#L1-L2",
                     std::string content = "bounded evidence") {
    return {std::move(source_id), std::move(content),
            agent::Value::object(
                {{"path", agent::Value("guide.md")},
                 {"line", agent::Value(std::int64_t{1})},
                 {"score", agent::Value(1.25)},
                 {"tags", agent::Value::array(
                              {agent::Value("rag"), agent::Value(true),
                               agent::Value()})}})};
}

agent::Value nested_metadata(std::size_t levels) {
    agent::Value value = agent::Value("leaf");
    for (std::size_t index = 0; index < levels; ++index) {
        value = agent::Value::array({std::move(value)});
    }
    return value;
}

}  // namespace fixtures

TEST_CASE(evidence_validation_accepts_empty_and_exact_bounded_packs) {
    REQUIRE(agent::evidence_pack_is_valid({}));
    REQUIRE(agent::evidence_pack_is_valid({{fixtures::item()}}));

    agent::EvidencePack exact;
    for (std::size_t index = 0; index < 4; ++index) {
        exact.items.push_back(fixtures::item(
            "source-" + std::to_string(index), std::string(8'192, 'x')));
    }
    REQUIRE(agent::evidence_pack_is_valid(exact));

    auto depth_sixteen = fixtures::item();
    depth_sixteen.metadata = fixtures::nested_metadata(15);
    REQUIRE(agent::evidence_pack_is_valid({{depth_sixteen}}));
}

TEST_CASE(evidence_validation_rejects_item_identity_and_content_violations) {
    agent::EvidencePack too_many;
    for (std::size_t index = 0; index < 21; ++index) {
        too_many.items.push_back(
            fixtures::item("source-" + std::to_string(index)));
    }
    REQUIRE(!agent::evidence_pack_is_valid(too_many));

    REQUIRE(!agent::evidence_pack_is_valid(
        {{fixtures::item("same"), fixtures::item("same")}}));
    REQUIRE(!agent::evidence_pack_is_valid({{fixtures::item("")}}));
    REQUIRE(!agent::evidence_pack_is_valid(
        {{fixtures::item("bad\nsource")}}));
    REQUIRE(!agent::evidence_pack_is_valid(
        {{fixtures::item(std::string(513, 's'))}}));
    REQUIRE(!agent::evidence_pack_is_valid(
        {{fixtures::item(std::string(1, static_cast<char>(0xFF)))}}));
    REQUIRE(!agent::evidence_pack_is_valid(
        {{fixtures::item("source", "")}}));
    REQUIRE(!agent::evidence_pack_is_valid(
        {{fixtures::item("source", std::string(8'193, 'x'))}}));
    REQUIRE(!agent::evidence_pack_is_valid(
        {{fixtures::item("source", std::string("a\0b", 3))}}));
    REQUIRE(!agent::evidence_pack_is_valid(
        {{fixtures::item("source",
                         std::string(1, static_cast<char>(0xFF)))}}));

    agent::EvidencePack total_too_large;
    for (std::size_t index = 0; index < 5; ++index) {
        total_too_large.items.push_back(fixtures::item(
            "source-" + std::to_string(index), std::string(7'000, 'x')));
    }
    REQUIRE(!agent::evidence_pack_is_valid(total_too_large));
}

TEST_CASE(evidence_validation_rejects_unencodable_or_unbounded_metadata) {
    auto nonfinite = fixtures::item();
    nonfinite.metadata = agent::Value(
        std::numeric_limits<double>::quiet_NaN());
    REQUIRE(!agent::evidence_pack_is_valid({{nonfinite}}));

    auto invalid_string = fixtures::item();
    invalid_string.metadata = agent::Value(
        std::string(1, static_cast<char>(0xFF)));
    REQUIRE(!agent::evidence_pack_is_valid({{invalid_string}}));

    auto nul_string = fixtures::item();
    nul_string.metadata = agent::Value(std::string("a\0b", 3));
    REQUIRE(!agent::evidence_pack_is_valid({{nul_string}}));

    auto oversized_strings = fixtures::item();
    oversized_strings.metadata = agent::Value(std::string(4'097, 'x'));
    REQUIRE(!agent::evidence_pack_is_valid({{oversized_strings}}));

    auto invalid_key = fixtures::item();
    invalid_key.metadata = agent::Value::object(
        {{std::string(1, static_cast<char>(0xFF)), agent::Value(true)}});
    REQUIRE(!agent::evidence_pack_is_valid({{invalid_key}}));

    auto too_deep = fixtures::item();
    too_deep.metadata = fixtures::nested_metadata(16);
    REQUIRE(!agent::evidence_pack_is_valid({{too_deep}}));

    agent::Value::Array nodes;
    nodes.reserve(256);
    for (std::size_t index = 0; index < 256; ++index) {
        nodes.emplace_back();
    }
    auto too_many_nodes = fixtures::item();
    too_many_nodes.metadata = agent::Value::array(std::move(nodes));
    REQUIRE(!agent::evidence_pack_is_valid({{too_many_nodes}}));
}

#include "adapters/workspace/workspace_path_policy.h"
#include "adapters/workspace/workspace_text.h"
#include "test_support.h"

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

namespace {

template <typename T>
const agent::workspace::Fault& fault(const agent::workspace::Outcome<T>& outcome) {
    return std::get<agent::workspace::Fault>(outcome);
}

template <typename T>
const T& value(const agent::workspace::Outcome<T>& outcome) {
    return std::get<T>(outcome);
}

}  // namespace

TEST_CASE(workspace_sha256_and_utf8_are_deterministic) {
    REQUIRE(agent::workspace::sha256_hex("") ==
            "e3b0c44298fc1c149afbf4c8996fb924"
            "27ae41e4649b934ca495991b7852b855");
    REQUIRE(agent::workspace::sha256_hex("abc") ==
            "ba7816bf8f01cfea414140de5dae2223"
            "b00361a396177a9cb410ff61f20015ad");
    REQUIRE(agent::workspace::is_strict_utf8_text(u8"中文🙂\n"));
    REQUIRE(agent::workspace::is_strict_utf8_text(""));
    REQUIRE(!agent::workspace::is_strict_utf8_text(std::string("a\0b", 3)));
    REQUIRE(!agent::workspace::is_strict_utf8_text("\xC0\xAF"));
    REQUIRE(!agent::workspace::is_strict_utf8_text("\xED\xA0\x80"));
}

TEST_CASE(workspace_line_pages_preserve_bytes_and_report_the_next_line) {
    const auto first = agent::workspace::line_page("a\r\nb\nc", 1, 2, 64);
    REQUIRE(std::holds_alternative<agent::workspace::LinePage>(first));
    REQUIRE(value(first).start_line == 1);
    REQUIRE(value(first).end_line == 2);
    REQUIRE(value(first).total_lines == 3);
    REQUIRE(value(first).content == "a\r\nb\n");
    REQUIRE(value(first).next_start_line == std::optional<std::size_t>{3});

    const auto last = agent::workspace::line_page("a\r\nb\nc", 3, 20, 64);
    REQUIRE(std::holds_alternative<agent::workspace::LinePage>(last));
    REQUIRE(value(last).content == "c");
    REQUIRE(!value(last).next_start_line.has_value());

    const auto empty = agent::workspace::line_page("", 1, 20, 64);
    REQUIRE(std::holds_alternative<agent::workspace::LinePage>(empty));
    REQUIRE(value(empty).total_lines == 0);
    REQUIRE(value(empty).end_line == 0);
    REQUIRE(value(empty).content.empty());
}

TEST_CASE(workspace_line_pages_reject_invalid_ranges_and_oversized_lines) {
    const auto bad_start = agent::workspace::line_page("one\n", 2, 1, 64);
    REQUIRE(std::holds_alternative<agent::workspace::Fault>(bad_start));
    REQUIRE(fault(bad_start).code ==
            agent::workspace::FaultCode::InvalidArguments);

    const auto zero_lines = agent::workspace::line_page("one\n", 1, 0, 64);
    REQUIRE(std::holds_alternative<agent::workspace::Fault>(zero_lines));
    REQUIRE(fault(zero_lines).code ==
            agent::workspace::FaultCode::InvalidArguments);

    const auto long_line = agent::workspace::line_page("12345\n", 1, 1, 4);
    REQUIRE(std::holds_alternative<agent::workspace::Fault>(long_line));
    REQUIRE(fault(long_line).code ==
            agent::workspace::FaultCode::LimitExceeded);
}

TEST_CASE(workspace_literal_matching_is_nonoverlapping_and_ascii_folded_only) {
    REQUIRE((agent::workspace::literal_matches("aaaa", "aa", true) ==
             std::vector<std::size_t>{0, 2}));
    REQUIRE((agent::workspace::literal_matches("Ababa", "aba", false) ==
             std::vector<std::size_t>{0}));
    REQUIRE((agent::workspace::literal_matches("[x]+ [x]+", "[x]+", true) ==
             std::vector<std::size_t>{0, 5}));
    REQUIRE(agent::workspace::literal_matches(u8"Ä", u8"ä", false).empty());
}

TEST_CASE(workspace_relative_paths_reject_escape_and_platform_aliases) {
    agent::workspace::WorkspacePathPolicy policy("runtime_data");
    const std::vector<std::string> rejected{
        "", "../x", "a/../x", "/x", "C:/x", "a\\b", "a//b",
        "a/./b", "a/", std::string("a\0b", 3)};
    for (const auto& path : rejected) {
        const auto parsed = policy.parse(path);
        REQUIRE(std::holds_alternative<agent::workspace::Fault>(parsed));
        REQUIRE(fault(parsed).code ==
                agent::workspace::FaultCode::InvalidArguments);
    }
#if defined(_WIN32)
    for (const auto& path : {"NUL", "NUL.txt", "COM1.log", "a:stream",
                             "trailing.", "trailing "}) {
        REQUIRE(std::holds_alternative<agent::workspace::Fault>(
            policy.parse(path)));
    }
#endif
    const auto root = policy.parse(".");
    REQUIRE(std::holds_alternative<agent::workspace::RelativePath>(root));
    REQUIRE(value(root).components.empty());
    const auto valid = policy.parse(u8"源/文件.cpp");
    REQUIRE(std::holds_alternative<agent::workspace::RelativePath>(valid));
    REQUIRE(value(valid).generic == u8"源/文件.cpp");
}

TEST_CASE(workspace_policy_blocks_metadata_secrets_and_runtime_data) {
    test::ScopedTempDir temp("workspace-policy");
    temp.write_text(".env", "SECRET=value\n");
    temp.write_text(".env.example", "NAME=value\n");
    temp.write_text(".git/config", "ignored\n");
    temp.write_text("runtime_data/events.jsonl", "ignored\n");
    temp.write_text("safe.txt", "ok\n");
    agent::workspace::WorkspacePathPolicy policy(temp.path() / "runtime_data");

    for (const auto& path : {".env", ".git/config", "runtime_data/events.jsonl"}) {
        const auto parsed = policy.parse(path);
        REQUIRE(std::holds_alternative<agent::workspace::RelativePath>(parsed));
        const auto resolved = policy.resolve_existing(
            temp.path(), value(parsed), false);
        REQUIRE(std::holds_alternative<agent::workspace::Fault>(resolved));
        REQUIRE(fault(resolved).code ==
                agent::workspace::FaultCode::AccessDenied);
        REQUIRE(fault(resolved).message.find(temp.path().generic_u8string()) ==
                std::string::npos);
    }

    const auto example = policy.parse(".env.example");
    const auto resolved_example = policy.resolve_existing(
        temp.path(), value(example), false);
    REQUIRE(std::holds_alternative<std::filesystem::path>(resolved_example));

    const auto safe = policy.parse("safe.txt");
    const auto resolved_safe = policy.resolve_existing(
        temp.path(), value(safe), false);
    REQUIRE(std::holds_alternative<std::filesystem::path>(resolved_safe));
}

TEST_CASE(workspace_policy_requires_the_requested_leaf_kind_and_existing_parent) {
    test::ScopedTempDir temp("workspace-kind");
    temp.write_text("dir/file.txt", "ok\n");
    agent::workspace::WorkspacePathPolicy policy(temp.path() / "runtime_data");

    const auto directory = policy.parse("dir");
    REQUIRE(std::holds_alternative<std::filesystem::path>(
        policy.resolve_existing(temp.path(), value(directory), true)));
    REQUIRE(std::holds_alternative<agent::workspace::Fault>(
        policy.resolve_existing(temp.path(), value(directory), false)));

    const auto file = policy.parse("dir/file.txt");
    REQUIRE(std::holds_alternative<agent::workspace::Fault>(
        policy.resolve_existing(temp.path(), value(file), true)));
    REQUIRE(std::holds_alternative<std::filesystem::path>(
        policy.resolve_parent(temp.path(), value(file))));

    const auto missing_parent = policy.parse("missing/file.txt");
    const auto parent = policy.resolve_parent(temp.path(), value(missing_parent));
    REQUIRE(std::holds_alternative<agent::workspace::Fault>(parent));
    REQUIRE(fault(parent).code == agent::workspace::FaultCode::NotFound);
}

TEST_CASE(workspace_policy_rejects_linked_roots_and_canonical_runtime_aliases) {
    test::ScopedTempDir temp("workspace-root-link-policy");
    const auto physical = temp.path() / "physical";
    const auto alias = temp.path() / "alias";
    std::filesystem::create_directories(physical / "private_runtime");
    {
        std::ofstream(physical / "safe.txt", std::ios::binary) << "safe\n";
        std::ofstream(physical / "private_runtime/secret.txt", std::ios::binary)
            << "private\n";
    }
    std::error_code link_error;
    std::filesystem::create_directory_symlink(physical, alias, link_error);
    if (link_error) {
        std::cout << "SKIP workspace root-link policy: environment cannot "
                     "create a directory symlink\n";
        return;
    }

    agent::workspace::WorkspacePathPolicy root_policy(
        temp.path() / "runtime_data");
    const auto safe = root_policy.parse("safe.txt");
    const auto linked_root =
        root_policy.resolve_existing(alias, value(safe), false);
    REQUIRE(std::holds_alternative<agent::workspace::Fault>(linked_root));
    REQUIRE(fault(linked_root).code ==
            agent::workspace::FaultCode::AccessDenied);

    agent::workspace::WorkspacePathPolicy runtime_alias_policy(
        alias / "private_runtime");
    const auto secret = runtime_alias_policy.parse("private_runtime/secret.txt");
    const auto protected_runtime = runtime_alias_policy.resolve_existing(
        physical, value(secret), false);
    REQUIRE(std::holds_alternative<agent::workspace::Fault>(protected_runtime));
    REQUIRE(fault(protected_runtime).code ==
            agent::workspace::FaultCode::AccessDenied);
}

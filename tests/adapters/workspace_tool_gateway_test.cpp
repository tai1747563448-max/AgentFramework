#include "adapters/workspace/workspace_tool_gateway.h"
#include "ports/tool_gateway.h"
#include "test_support.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <utility>

namespace fixtures {

agent::ToolCall list_call(std::string id,
                          agent::Value path,
                          bool recursive = false,
                          std::int64_t max_results = 100) {
    return {std::move(id), "list_files",
            agent::Value::object({
                {"path", std::move(path)},
                {"recursive", recursive},
                {"max_results", max_results}})};
}

agent::ToolCall read_call(std::string id,
                          agent::Value path,
                          std::int64_t start_line = 1,
                          std::int64_t max_lines = 200) {
    return {std::move(id), "read_file",
            agent::Value::object({
                {"path", std::move(path)},
                {"start_line", start_line},
                {"max_lines", max_lines}})};
}

agent::ToolCall search_call(std::string id,
                            std::string path,
                            std::string query,
                            bool case_sensitive = true,
                            std::int64_t max_results = 100) {
    return {std::move(id), "search_text",
            agent::Value::object({
                {"path", std::move(path)},
                {"query", std::move(query)},
                {"case_sensitive", case_sensitive},
                {"max_results", max_results}})};
}

nlohmann::json content_json(const agent::Result<agent::ToolResult>& result) {
    REQUIRE(result.has_value());
    return nlohmann::json::parse(result.value().content);
}

std::string error_code(const agent::Result<agent::ToolResult>& result) {
    REQUIRE(result.has_value());
    REQUIRE(result.value().is_error);
    return content_json(result).at("error").at("code").get<std::string>();
}

}  // namespace fixtures

TEST_CASE(workspace_gateway_exposes_exact_five_closed_schemas) {
    agent::WorkspaceToolGateway gateway("runtime_data");
    const auto definitions = gateway.definitions();
    REQUIRE(definitions.size() == 5);
    REQUIRE(definitions.at(0).name == "list_files");
    REQUIRE(definitions.at(1).name == "read_file");
    REQUIRE(definitions.at(2).name == "search_text");
    REQUIRE(definitions.at(3).name == "replace_text");
    REQUIRE(definitions.at(4).name == "write_file");
    for (const auto& definition : definitions) {
        REQUIRE(definition.input_schema.at("type").as_string() == "object");
        REQUIRE(!definition.input_schema.at("additionalProperties").as_bool());
        REQUIRE(definition.input_schema.at("properties").is_object());
        REQUIRE(definition.input_schema.at("required").is_array());
    }
}

TEST_CASE(workspace_gateway_returns_bounded_errors_for_unknown_or_bad_arguments) {
    test::ScopedTempDir temp("workspace-bad-args");
    agent::WorkspaceToolGateway gateway(temp.path() / "runtime_data");
    const agent::ToolExecutionContext context{temp.path().generic_u8string()};

    const auto unknown = gateway.execute(
        {"call-unknown", "delete_file", agent::Value::object({})}, context);
    REQUIRE(fixtures::error_code(unknown) == "invalid_arguments");
    REQUIRE(unknown.value().tool_call_id == "call-unknown");

    const auto wrong_type = gateway.execute(
        fixtures::list_call("call-type", agent::Value(std::int64_t{7})),
        context);
    REQUIRE(fixtures::error_code(wrong_type) == "invalid_arguments");

    const auto extra = gateway.execute(
        {"call-extra", "list_files",
         agent::Value::object({{"path", "."},
                               {"recursive", false},
                               {"max_results", std::int64_t{100}},
                               {"unexpected", true}})},
        context);
    REQUIRE(fixtures::error_code(extra) == "invalid_arguments");

    const auto bad_range = gateway.execute(
        fixtures::read_call("call-range", "missing.txt", 0, 200), context);
    REQUIRE(fixtures::error_code(bad_range) == "invalid_arguments");
    REQUIRE(unknown.value().content.size() < 256);
}

TEST_CASE(workspace_list_is_relative_sorted_and_omits_protected_entries) {
    test::ScopedTempDir temp(std::filesystem::u8path(u8"工作区-list"));
    temp.write_text("b.txt", "b\n");
    temp.write_text("a.txt", "a\n");
    temp.write_text(std::filesystem::u8path(u8"源/c.txt"), "c\n");
    temp.write_text(".env", "SECRET=value\n");
    temp.write_text(".git/config", "private\n");
    temp.write_text("runtime_data/events.jsonl", "private\n");
    agent::WorkspaceToolGateway gateway(temp.path() / "runtime_data");
    const agent::ToolExecutionContext context{temp.path().generic_u8string()};

    const auto listed = gateway.execute(
        fixtures::list_call("call-list", ".", true, 100), context);

    REQUIRE(listed.has_value());
    REQUIRE(!listed.value().is_error);
    REQUIRE(listed.value().content.size() <= 65536);
    const auto json = fixtures::content_json(listed);
    REQUIRE(json.at("path") == ".");
    REQUIRE(json.at("truncated") == false);
    REQUIRE(json.at("omitted_entries").get<std::size_t>() >= 3);
    const auto& entries = json.at("entries");
    REQUIRE(entries.size() == 4);
    REQUIRE(entries.at(0).at("path") == "a.txt");
    REQUIRE(entries.at(1).at("path") == "b.txt");
    REQUIRE(entries.at(2).at("path") == u8"源");
    REQUIRE(entries.at(3).at("path") == u8"源/c.txt");
    REQUIRE(listed.value().content.find(temp.path().generic_u8string()) ==
            std::string::npos);
    REQUIRE(listed.value().content.find(".env") == std::string::npos);
    REQUIRE(listed.value().content.find(".git") == std::string::npos);
    REQUIRE(listed.value().content.find("events.jsonl") == std::string::npos);
}

TEST_CASE(workspace_list_marks_max_result_truncation) {
    test::ScopedTempDir temp("workspace-list-limit");
    temp.write_text("a.txt", "a\n");
    temp.write_text("b.txt", "b\n");
    temp.write_text("c.txt", "c\n");
    agent::WorkspaceToolGateway gateway(temp.path() / "runtime_data");
    const agent::ToolExecutionContext context{temp.path().generic_u8string()};

    const auto listed = gateway.execute(
        fixtures::list_call("call-list-limit", ".", false, 2), context);
    const auto json = fixtures::content_json(listed);
    REQUIRE(!listed.value().is_error);
    REQUIRE(json.at("entries").size() == 2);
    REQUIRE(json.at("truncated") == true);
}

TEST_CASE(workspace_read_preserves_crlf_pages_and_full_file_hash) {
    test::ScopedTempDir temp(std::filesystem::u8path(u8"工作区-read"));
    temp.write_text(std::filesystem::u8path(u8"源/a.cpp"),
                    "abc\r\ntwo\nthree");
    agent::WorkspaceToolGateway gateway(temp.path() / "runtime_data");
    const agent::ToolExecutionContext context{temp.path().generic_u8string()};

    const auto read = gateway.execute(
        fixtures::read_call("call-read", u8"源/a.cpp", 1, 2), context);

    REQUIRE(read.has_value());
    REQUIRE(!read.value().is_error);
    const auto json = fixtures::content_json(read);
    REQUIRE(json.at("path") == u8"源/a.cpp");
    REQUIRE(json.at("sha256") ==
            "a3eeca252ec12eaa4e258f3204affb86"
            "aae763058078771dd6536b478e0be180");
    REQUIRE(json.at("start_line") == 1);
    REQUIRE(json.at("end_line") == 2);
    REQUIRE(json.at("total_lines") == 3);
    REQUIRE(json.at("content") == "abc\r\ntwo\n");
    REQUIRE(json.at("truncated") == true);
    REQUIRE(json.at("next_start_line") == 3);
}

TEST_CASE(workspace_read_handles_empty_and_rejects_oversized_or_invalid_text) {
    test::ScopedTempDir temp("workspace-read-limits");
    temp.write_text("empty.txt", "");
    temp.write_text("invalid.txt", std::string("a\0b", 3));
    temp.write_text("long-line.txt", std::string(70 * 1024, 'x'));
    std::string exact_limit_text;
    exact_limit_text.reserve(1024 * 1024);
    for (std::size_t index = 0; index < 512 * 1024; ++index) {
        exact_limit_text += "x\n";
    }
    temp.write_text("limit.txt", exact_limit_text);
    temp.write_text("too-large.txt", std::string(1024 * 1024 + 1, 'z'));
    agent::WorkspaceToolGateway gateway(temp.path() / "runtime_data");
    const agent::ToolExecutionContext context{temp.path().generic_u8string()};

    const auto empty = gateway.execute(
        fixtures::read_call("call-empty", "empty.txt"), context);
    const auto empty_json = fixtures::content_json(empty);
    REQUIRE(!empty.value().is_error);
    REQUIRE(empty_json.at("total_lines") == 0);
    REQUIRE(empty_json.at("content") == "");

    REQUIRE(fixtures::error_code(gateway.execute(
                fixtures::read_call("call-invalid", "invalid.txt"), context)) ==
            "unsupported_file");
    REQUIRE(fixtures::error_code(gateway.execute(
                fixtures::read_call("call-long", "long-line.txt"), context)) ==
            "limit_exceeded");
    const auto exact_limit = gateway.execute(
        fixtures::read_call("call-limit", "limit.txt", 2, 1), context);
    REQUIRE(exact_limit.has_value() && !exact_limit.value().is_error);
    REQUIRE(exact_limit.value().content.size() <= 65536);
    REQUIRE(fixtures::error_code(gateway.execute(
                fixtures::read_call("call-large", "too-large.txt"), context)) ==
            "limit_exceeded");
}

TEST_CASE(workspace_read_rejects_hard_links_and_does_not_follow_symlinks) {
    test::ScopedTempDir temp("workspace-link-guard");
    const auto original = temp.write_text("original.txt", "sentinel\n");
    const auto alias = temp.path() / "alias.txt";
    std::error_code hard_link_error;
    std::filesystem::create_hard_link(original, alias, hard_link_error);
    REQUIRE(!hard_link_error);

    agent::WorkspaceToolGateway gateway(temp.path() / "runtime_data");
    const agent::ToolExecutionContext context{temp.path().generic_u8string()};
    REQUIRE(fixtures::error_code(gateway.execute(
                fixtures::read_call("call-hard", "alias.txt"), context)) ==
            "access_denied");

    test::ScopedTempDir outside("workspace-link-outside");
    const auto outside_file = outside.write_text("secret.txt", "outside\n");
    const auto link = temp.path() / "link.txt";
    std::error_code symlink_error;
    std::filesystem::create_symlink(outside_file, link, symlink_error);
    if (symlink_error) {
        std::cout << "SKIP workspace symlink guard: environment cannot create "
                     "a file symlink\n";
    } else {
        REQUIRE(fixtures::error_code(gateway.execute(
                    fixtures::read_call("call-link", "link.txt"), context)) ==
                "access_denied");
    }
}

TEST_CASE(workspace_search_is_literal_stable_unicode_aware_and_non_overlapping) {
    test::ScopedTempDir temp("workspace-search-semantics");
    temp.write_text("a.cpp", "toolgateway [x]+\n");
    temp.write_text("b.cpp", "ToolGateway ToolGateway\n[x]+\n");
    temp.write_text(std::filesystem::u8path(u8"源.cpp"),
                    u8"你🙂ToolGateway\naaaa\n");
    agent::WorkspaceToolGateway gateway(temp.path() / "runtime_data");
    const agent::ToolExecutionContext context{temp.path().generic_u8string()};

    const auto exact = gateway.execute(
        fixtures::search_call("call-exact", ".", "ToolGateway", true, 10),
        context);
    REQUIRE(exact.has_value() && !exact.value().is_error);
    const auto exact_json = fixtures::content_json(exact);
    REQUIRE(exact_json.at("matches").size() == 3);
    REQUIRE(exact_json.at("matches").at(0).at("path") == "b.cpp");
    REQUIRE(exact_json.at("matches").at(0).at("line") == 1);
    REQUIRE(exact_json.at("matches").at(0).at("column") == 1);
    REQUIRE(exact_json.at("matches").at(1).at("path") == "b.cpp");
    REQUIRE(exact_json.at("matches").at(1).at("column") == 13);
    REQUIRE(exact_json.at("matches").at(2).at("path") == u8"源.cpp");
    REQUIRE(exact_json.at("matches").at(2).at("column") == 3);
    REQUIRE(exact_json.at("truncated") == false);

    const auto folded = gateway.execute(
        fixtures::search_call("call-folded", ".", "TOOLGATEWAY", false, 10),
        context);
    REQUIRE(fixtures::content_json(folded).at("matches").size() == 4);

    const auto literal = gateway.execute(
        fixtures::search_call("call-literal", ".", "[x]+", true, 10),
        context);
    const auto literal_json = fixtures::content_json(literal);
    REQUIRE(literal_json.at("matches").size() == 2);
    REQUIRE(literal_json.at("matches").at(0).at("path") == "a.cpp");
    REQUIRE(literal_json.at("matches").at(1).at("path") == "b.cpp");

    const auto non_overlapping = gateway.execute(
        fixtures::search_call("call-non-overlap", u8"源.cpp", "aa", true, 10),
        context);
    const auto non_overlapping_json = fixtures::content_json(non_overlapping);
    REQUIRE(non_overlapping_json.at("matches").size() == 2);
    REQUIRE(non_overlapping_json.at("matches").at(0).at("column") == 1);
    REQUIRE(non_overlapping_json.at("matches").at(1).at("column") == 3);
}

TEST_CASE(workspace_search_omits_unsafe_directory_entries_and_errors_on_leaf) {
    test::ScopedTempDir temp("workspace-search-unsafe");
    temp.write_text("ok.txt", "needle\nneedle\nneedle\n");
    temp.write_text("invalid.txt", std::string("needle\0hidden", 13));
    temp.write_text(".env", "needle=secret\n");
    temp.write_text(".git/config", "needle=private\n");
    const auto original = temp.write_text("original.txt", "needle\n");
    std::error_code link_error;
    std::filesystem::create_hard_link(original, temp.path() / "alias.txt",
                                      link_error);
    REQUIRE(!link_error);
    agent::WorkspaceToolGateway gateway(temp.path() / "runtime_data");
    const agent::ToolExecutionContext context{temp.path().generic_u8string()};

    const auto directory = gateway.execute(
        fixtures::search_call("call-directory", ".", "needle", true, 2),
        context);
    REQUIRE(directory.has_value() && !directory.value().is_error);
    const auto directory_json = fixtures::content_json(directory);
    REQUIRE(directory_json.at("matches").size() == 2);
    REQUIRE(directory_json.at("truncated") == true);
    REQUIRE(directory_json.at("truncation_reason") == "max_results");
    REQUIRE(directory_json.at("omitted_entries").get<std::size_t>() >= 4);
    REQUIRE(directory.value().content.find("invalid.txt") == std::string::npos);
    REQUIRE(directory.value().content.find(".env") == std::string::npos);
    REQUIRE(directory.value().content.find(".git") == std::string::npos);

    REQUIRE(fixtures::error_code(gateway.execute(
                fixtures::search_call("call-invalid-leaf", "invalid.txt",
                                      "needle"),
                context)) == "unsupported_file");
    REQUIRE(fixtures::error_code(gateway.execute(
                fixtures::search_call("call-hard-leaf", "alias.txt", "needle"),
                context)) == "access_denied");
    REQUIRE(fixtures::error_code(gateway.execute(
                fixtures::search_call("call-empty-query", ".", ""), context)) ==
            "invalid_arguments");
}

TEST_CASE(workspace_search_reports_entry_file_and_byte_scan_budgets) {
    {
        test::ScopedTempDir temp("workspace-search-entry-budget");
        for (std::size_t index = 0; index < 2001; ++index) {
            std::filesystem::create_directory(
                temp.path() / ("dir-" + std::to_string(index)));
        }
        agent::WorkspaceToolGateway gateway(temp.path() / "runtime_data");
        const agent::ToolExecutionContext context{temp.path().generic_u8string()};
        const auto result = gateway.execute(
            fixtures::search_call("call-entry-budget", ".", "absent"),
            context);
        const auto json = fixtures::content_json(result);
        REQUIRE(!result.value().is_error);
        REQUIRE(json.at("truncated") == true);
        REQUIRE(json.at("truncation_reason") == "entry_budget");
    }
    {
        test::ScopedTempDir temp("workspace-search-file-budget");
        for (std::size_t index = 0; index < 501; ++index) {
            temp.write_text("file-" + std::to_string(index) + ".txt", "x\n");
        }
        agent::WorkspaceToolGateway gateway(temp.path() / "runtime_data");
        const agent::ToolExecutionContext context{temp.path().generic_u8string()};
        const auto result = gateway.execute(
            fixtures::search_call("call-file-budget", ".", "absent"), context);
        const auto json = fixtures::content_json(result);
        REQUIRE(json.at("scanned_files") == 500);
        REQUIRE(json.at("truncated") == true);
        REQUIRE(json.at("truncation_reason") == "file_budget");
    }
    {
        test::ScopedTempDir temp("workspace-search-byte-budget");
        const std::string one_mebibyte(1024 * 1024, 'x');
        for (std::size_t index = 0; index < 17; ++index) {
            temp.write_text("file-" + std::to_string(index) + ".txt",
                            one_mebibyte);
        }
        agent::WorkspaceToolGateway gateway(temp.path() / "runtime_data");
        const agent::ToolExecutionContext context{temp.path().generic_u8string()};
        const auto result = gateway.execute(
            fixtures::search_call("call-byte-budget", ".", "absent"), context);
        const auto json = fixtures::content_json(result);
        REQUIRE(json.at("scanned_files") == 16);
        REQUIRE(json.at("truncated") == true);
        REQUIRE(json.at("truncation_reason") == "byte_budget");
    }
}

TEST_CASE(workspace_search_stops_before_the_serialized_result_limit) {
    test::ScopedTempDir temp("workspace-search-output-budget");
    const std::string long_line = "needle" + std::string(600, 'x') + "\n";
    for (std::size_t index = 0; index < 200; ++index) {
        temp.write_text("file-" + std::to_string(index) + ".txt", long_line);
    }
    agent::WorkspaceToolGateway gateway(temp.path() / "runtime_data");
    const agent::ToolExecutionContext context{temp.path().generic_u8string()};
    const auto result = gateway.execute(
        fixtures::search_call("call-output-budget", ".", "needle", true, 200),
        context);

    REQUIRE(result.has_value() && !result.value().is_error);
    REQUIRE(result.value().content.size() <= 65536);
    const auto json = fixtures::content_json(result);
    REQUIRE(json.at("matches").size() < 200);
    REQUIRE(json.at("truncated") == true);
    REQUIRE(json.at("truncation_reason") == "output_bytes");
}

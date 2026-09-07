#include "application/memory_policy.h"
#include "test_support.h"

#include <string>
#include <vector>

TEST_CASE(memory_policy_rejects_sensitive_candidates_without_echoing_them) {
    const std::string configured_credential = "provider-credential-123456";
    const agent::MemoryPolicy policy({64, {configured_credential}});
    const std::vector<std::string> sensitive{
        configured_credential,
        "prefix " + configured_credential + " suffix",
        "sk-1234567890abcdefghijklmnopqrstuvwxyz",
        "sk_1234567890abcdefghijklmnopqrstuvwxyz",
        "Bearer abcdefghijklmnopqrstuvwxyz",
        "password=correct-horse-battery-staple",
        "DB_PASSWORD=correct-horse-battery-staple",
        "secret: nontrivial-secret-value",
        "CLIENT_SECRET: should-not-persist-1234567890",
        "CLIENT_SECRET=nontrivial-secret-value",
        "client-secret=nontrivial-secret-value",
        "api-key = nontrivial-api-key-value",
        "api_key: must-not-leak-1234567890",
        "OPENAI_API_KEY=nontrivial-api-key-value",
        "password: \"quoted-token-1234567890\"",
        "password: contiguousvalue",
        "password: correct horse battery staple",
        "password: d4nger-1234567890 commentary",
        "CLIENT_SECRET: alpha beta gamma",
        "-----BEGIN PRIVATE KEY-----",
        "-----BEGIN RSA PRIVATE KEY-----"};
    for (const auto& candidate : sensitive) {
        const auto result = policy.validate_candidate(candidate);
        REQUIRE(!result.has_value());
        REQUIRE(result.error().message.find(candidate) == std::string::npos);
    }
}

TEST_CASE(memory_policy_rejects_secret_colon_value_after_short_first_word) {
    const agent::MemoryPolicy policy({4096, {}});
    REQUIRE(!policy.validate_candidate("CLIENT_SECRET: abc longsecretvalue").has_value());
}

TEST_CASE(memory_policy_rejects_password_colon_value_after_short_first_word) {
    const agent::MemoryPolicy policy({4096, {}});
    REQUIRE(!policy.validate_candidate("password: abc 1234567890").has_value());
}

TEST_CASE(memory_policy_rejects_quoted_serialized_credential_fields) {
    const agent::MemoryPolicy policy({4096, {}});
    const std::vector<std::string> serialized_credentials{
        R"({"password":"ordinaryfixture123"})",
        R"({ "secret" : "ordinary fixture 123" })",
        R"({'api_key' : 'ordinaryfixture123'})"};

    for (const auto& candidate : serialized_credentials)
        REQUIRE(!policy.validate_candidate(candidate).has_value());
}

TEST_CASE(memory_policy_accepts_ordinary_text_and_enforces_exact_byte_boundary) {
    const agent::MemoryPolicy ordinary_policy({4096, {}});
    const agent::MemoryPolicy bounded_policy({8, {}});
    REQUIRE(ordinary_policy.validate_candidate("password policies need review").has_value());
    REQUIRE(ordinary_policy
                .validate_candidate("Password: should contain twelve characters")
                .has_value());
    REQUIRE(ordinary_policy
                .validate_candidate("Password: recommended length is twelve characters")
                .has_value());
    REQUIRE(ordinary_policy
                .validate_candidate("Password: use at least 12 characters")
                .has_value());
    REQUIRE(ordinary_policy
                .validate_candidate("Password: prefer a long, user-chosen phrase")
                .has_value());
    REQUIRE(ordinary_policy
                .validate_candidate("Review the 'password' policy before release")
                .has_value());
    REQUIRE(ordinary_policy.validate_candidate("请用中文记录普通偏好").has_value());
    REQUIRE(bounded_policy.validate_candidate("12345678").has_value());
    REQUIRE(!bounded_policy.validate_candidate("123456789").has_value());
    REQUIRE(!ordinary_policy.validate_candidate(" \t\r\n ").has_value());
    REQUIRE(!ordinary_policy.validate_candidate("\xE2\x80\x83").has_value());
    REQUIRE(!ordinary_policy.validate_candidate("\xC0\xAF").has_value());
}

TEST_CASE(memory_policy_recognizes_turn_local_memory_opt_out_phrases) {
    const std::vector<std::string> phrases{
        "不要使用记忆", "不要参考记忆", "别参考历史", "忽略之前的记忆",
        "DO NOT USE MEMORY", "Don't use memory", "IGNORE PREVIOUS MEMORY",
        "ignore memories"};
    for (const auto& phrase : phrases) {
        REQUIRE(agent::memory_opted_out("Please " + phrase + " for this turn."));
    }
    REQUIRE(!agent::memory_opted_out("Please remember my password policy."));
}

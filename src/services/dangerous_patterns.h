#pragma once

// T21 (v2 §3): proactive dangerous-pattern detection. The runtime
// inspects every model-requested tool call against a small set of
// substring / path heuristics before it reaches the gateway. When a
// pattern matches, the call is cancelled with reason="dangerous_pattern"
// and the presenter prints "agent refuses to run this action; allow?".
//
// The five patterns:
//   - .ssh   (ssh private keys, known_hosts backdoor)
//   - .aws   (cloud credentials)
//   - rm -rf (destructive recursive delete)
//   - curl | sh (the textbook RCE chain)
//   - /etc/  (host config / privilege escalation)
//
// dangerous_patterns is the fail-closed cousin of T11's Permission:
// Permission asks the user for every ambiguous call; dangerous_patterns
// only blocks the obvious ones. They share the same hook chain (T13)
// and the same decision vocabulary (Allow / Deny).
#include <string>
#include <vector>

namespace agent {

class Value;

struct DangerousPattern {
    const char* id;             // stable identifier for logs
    const char* description;    // human-readable explanation
    const char* needle;         // substring match (case-sensitive)
    bool require_rooted;        // only match when the call argument
                                // resolves to a rooted / absolute path
};

inline const std::vector<DangerousPattern>& dangerous_pattern_table() {
    static const std::vector<DangerousPattern> kTable{
        {"ssh_keys",    "writes to ~/.ssh/",     ".ssh/",   true},
        {"aws_creds",   "writes to ~/.aws/",     ".aws/",   true},
        {"destructive", "rm -rf recursive",      "rm -rf",  false},
        {"curl_pipe_sh","curl | sh remote exec", "curl",    false},
        {"host_etc",    "writes under /etc/",    "/etc/",   true},
    };
    return kTable;
}

// match returns the first pattern whose needle appears in the joined
// call (input + name), or nullptr when none match. Rooted patterns
// require the matched needle to sit on a path that begins with '/' or
// a Windows drive letter, so plain text mentions of ".ssh" do not
// trip the heuristic.
//
// The Value overload serialises the structured arguments to JSON
// before scanning so that nested fields (e.g. {"command":"rm -rf /"})
// are matched the same way as flat strings.
const DangerousPattern* match_dangerous_pattern(
    const std::string& tool_name,
    const std::string& input_json);
const DangerousPattern* match_dangerous_pattern(
    const std::string& tool_name,
    const Value& input_value);

}  // namespace agent
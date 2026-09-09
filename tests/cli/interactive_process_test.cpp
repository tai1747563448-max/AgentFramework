#include "adapters/persistence/jsonl_event_store.h"
#include "adapters/persistence/jsonl_memory_store.h"
#include "adapters/persistence/jsonl_session_store.h"
#include "adapters/process/direct_process_runner.h"
#include "application/state_reducer.h"
#include "domain/session_event.h"
#include "test_support.h"

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef AGENT_INTERACTIVE_EXE_PATH
#error AGENT_INTERACTIVE_EXE_PATH must be defined
#endif

namespace test {

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

class SocketRuntime final {
public:
    SocketRuntime() {
#if defined(_WIN32)
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("failed to initialize loopback sockets");
        }
#endif
    }
    ~SocketRuntime() {
#if defined(_WIN32)
        WSACleanup();
#endif
    }
};

void close_socket(SocketHandle socket) {
#if defined(_WIN32)
    closesocket(socket);
#else
    close(socket);
#endif
}

int wait_for_socket(SocketHandle socket, std::chrono::milliseconds timeout) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(socket, &readable);
    timeval interval{};
    interval.tv_sec = static_cast<long>(timeout.count() / 1000);
    interval.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
#if defined(_WIN32)
    return select(0, &readable, nullptr, nullptr, &interval);
#else
    return select(socket + 1, &readable, nullptr, nullptr, &interval);
#endif
}

SocketHandle create_listener(std::uint16_t& port) {
    const auto listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kInvalidSocket) {
        throw std::runtime_error("failed to create loopback listener");
    }
    const int enabled = 1;
#if defined(_WIN32)
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&enabled), sizeof(enabled));
#else
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(0);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) != 0 ||
        listen(listener, 2) != 0) {
        close_socket(listener);
        throw std::runtime_error("failed to bind loopback listener");
    }
#if defined(_WIN32)
    int address_size = sizeof(address);
#else
    socklen_t address_size = sizeof(address);
#endif
    if (getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                    &address_size) != 0) {
        close_socket(listener);
        throw std::runtime_error("failed to inspect loopback listener");
    }
    port = ntohs(address.sin_port);
    return listener;
}

std::string receive_request(SocketHandle connection) {
    std::string request;
    std::size_t expected = std::string::npos;
    char buffer[4096];
    while (request.size() < expected) {
        if (wait_for_socket(connection, std::chrono::seconds(2)) != 1) {
            throw std::runtime_error("request body timed out");
        }
        const int received = recv(connection, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }
        request.append(buffer, static_cast<std::size_t>(received));
        const auto header_end = request.find("\r\n\r\n");
        if (header_end == std::string::npos || expected != std::string::npos) {
            continue;
        }
        auto headers = request.substr(0, header_end);
        std::transform(headers.begin(), headers.end(), headers.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        const std::string marker = "content-length:";
        const auto position = headers.find(marker);
        if (position == std::string::npos) {
            throw std::runtime_error("request has no content length");
        }
        const auto begin = position + marker.size();
        const auto end = headers.find("\r\n", begin);
        const auto length = static_cast<std::size_t>(
            std::stoull(headers.substr(begin, end - begin)));
        expected = header_end + 4 + length;
    }
    return request;
}

bool send_all(SocketHandle connection, const std::string& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const int sent = send(
            connection, bytes.data() + offset,
            static_cast<int>(bytes.size() - offset), 0);
        if (sent <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(sent);
    }
    return true;
}

nlohmann::json request_body(const std::string& request) {
    const auto body = request.find("\r\n\r\n");
    if (body == std::string::npos) {
        throw std::runtime_error("request has no body");
    }
    return nlohmann::json::parse(request.substr(body + 4));
}

std::string text_at(const nlohmann::json& message) {
    return message.at("content").at(0).at("text").get<std::string>();
}

std::string provider_response(const std::string& id,
                              const std::string& text) {
    const nlohmann::json body{
        {"id", id},
        {"content", {{{"type", "text"}, {"text", text}}}},
        {"stop_reason", "end_turn"},
        {"usage", {{"input_tokens", 12}, {"output_tokens", 3}}}};
    const auto encoded = body.dump();
    return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
           "Content-Length: " +
           std::to_string(encoded.size()) +
           "\r\nConnection: close\r\n\r\n" + encoded;
}

std::string read_all(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

std::string executable_under_test() {
    const auto* override_path = std::getenv("AGENT_PROCESS_TEST_EXE");
    return override_path != nullptr ? override_path : AGENT_INTERACTIVE_EXE_PATH;
}

enum class RequestKind { Normal, Compaction, Consolidation };

RequestKind classify_request(const nlohmann::json& request) {
    const auto system = request.at("system").get<std::string>();
    if (system.find("Create one cumulative plain-text conversation summary.") == 0)
        return RequestKind::Compaction;
    if (system.find("Extract concise durable memory candidates") == 0)
        return RequestKind::Consolidation;
    return RequestKind::Normal;
}

struct ExpectedRequest {
    RequestKind kind;
    std::string response;
};

// Record parsed bodies only; discard framing headers.
class ScriptedProvider final {
public:
    explicit ScriptedProvider(std::vector<ExpectedRequest> expected)
        : expected_(std::move(expected)), listener_(create_listener(port_)),
          worker_([this] { serve(); }) {}

    ~ScriptedProvider() { stop(); }
    std::uint16_t port() const { return port_; }
    void verify() {
        stop();
        if (!error_.empty() || requests.size() != expected_.size())
            throw std::runtime_error("scripted provider contract: expected=" +
                std::to_string(expected_.size()) + ", actual=" + std::to_string(requests.size()) +
                ", failure=" + error_);
    }
    std::vector<nlohmann::json> requests;

private:
    void stop() {
        stopping_.store(true);
        if (worker_.joinable()) worker_.join();
    }
    void serve() {
        SocketHandle connection = kInvalidSocket;
        try {
            while (!stopping_.load()) {
                const auto ready = wait_for_socket(listener_, std::chrono::milliseconds(25));
                if (ready == 0) continue;
                if (ready != 1) throw std::runtime_error("listener failed");
                connection = accept(listener_, nullptr, nullptr);
                if (connection == kInvalidSocket) throw std::runtime_error("accept failed");
                auto body = request_body(receive_request(connection));
                const auto kind = classify_request(body);
                const auto index = requests.size();
                requests.push_back(std::move(body));
                if (index >= expected_.size() || kind != expected_[index].kind)
                    throw std::runtime_error("unexpected request type or count");
                if (!send_all(connection, provider_response(
                        "scripted-" + std::to_string(index), expected_[index].response)))
                    throw std::runtime_error("send failed");
                close_socket(connection);
                connection = kInvalidSocket;
            }
        } catch (...) {
            error_ = "scripted loopback request contract failed";
        }
        if (connection != kInvalidSocket) close_socket(connection);
        close_socket(listener_);
    }

    SocketRuntime sockets_;
    std::vector<ExpectedRequest> expected_;
    std::uint16_t port_{0};
    SocketHandle listener_;
    std::atomic<bool> stopping_{false};
    std::string error_;
    std::thread worker_;
};

constexpr const char* kSyntheticCredential = "tasksevenopaquevalue";

std::string fixture_config(const std::filesystem::path& runtime,
                           std::uint16_t port, bool small_context = false) {
    return "AGENT_BASE_URL=http://127.0.0.1:" + std::to_string(port) + "\n"
        "AGENT_MODEL=MiniMax-M3\nAGENT_API_KEY=" + kSyntheticCredential + "\n"
        "AGENT_RUNTIME_ROOT=" + runtime.generic_u8string() + "\n"
        "AGENT_SYSTEM_PROMPT=process fixture base\n"
        "AGENT_ENABLE_MEMORY=1\nAGENT_MEMORY_TOP_K=1\n"
        "AGENT_MEMORY_MAX_INJECTED_BYTES=160\nAGENT_MEMORY_MAX_ENTRY_BYTES=128\n"
        "AGENT_ENABLE_BUILD_TOOLS=0\nAGENT_ENABLE_RAG=0\n"
        "AGENT_MODEL_TIMEOUT_SECONDS=3\nAGENT_COMPACTION_RETAIN_TURNS=1\n"
        "AGENT_COMPACTION_MAX_SUMMARY_BYTES=128\n"
        "AGENT_COMPACTION_THRESHOLD_BYTES=" + std::string(small_context ? "256" : "65536") + "\n"
        "AGENT_COMPACTION_HARD_LIMIT_BYTES=131072\n";
}

agent::ProcessOutput run_script(const std::filesystem::path& cwd,
                                const std::vector<std::string>& arguments,
                                const std::string& input,
                                const std::string& executable = executable_under_test()) {
    agent::DirectProcessRunner process;
    const auto result = process.run({executable, arguments, cwd, input,
        30'000, 256 * 1024, 64 * 1024, {}});
    REQUIRE(result.has_value());
    REQUIRE(!result.value().timed_out);
    REQUIRE(result.value().exit_code == 0);
    REQUIRE(result.value().stderr_utf8.empty());
    REQUIRE(!result.value().stdout_truncated);
    REQUIRE(!result.value().stderr_truncated);
    REQUIRE((result.value().stdout_utf8 + result.value().stderr_utf8).find(kSyntheticCredential) ==
            std::string::npos);
    return result.value();
}

std::string memory_section(const nlohmann::json& request) {
    const auto system = request.at("system").get<std::string>();
    const std::string marker = "--- BEGIN Long-term memories ---\n";
    const auto begin = system.find(marker);
    if (begin == std::string::npos) return {};
    const auto line_begin = system.find("\n- [", begin);
    const auto end = system.find("\n--- END Long-term memories ---", line_begin);
    REQUIRE(line_begin != std::string::npos);
    REQUIRE(end != std::string::npos);
    return system.substr(line_begin + 1, end - line_begin - 1);
}

void assert_memory_injection(const nlohmann::json& request,
                             const std::string& memory_id, bool present) {
    const auto section = memory_section(request);
    REQUIRE((!section.empty()) == present);
    REQUIRE(section.size() <= 160);
    REQUIRE((request.at("system").get<std::string>().find(memory_id) != std::string::npos) == present);
    if (present) {
        REQUIRE(section == "- [" + memory_id + "] (fact) Orion review uses offline checks.");
        REQUIRE(section.find('\n') == std::string::npos);
    }
}

template <typename Event>
void assert_sequences(const std::vector<Event>& events) {
    REQUIRE(!events.empty());
    for (std::size_t index = 0; index < events.size(); ++index)
        REQUIRE(events[index].sequence == index + 1);
}

}  // namespace test

TEST_CASE(interactive_executable_keeps_two_utf8_turns_and_linked_logs) {
    // Preserve case results if the outer process times out.
    std::cout << std::unitbuf;
    test::ScopedTempDir temp("interactive-process");
    const auto workspace = temp.path() / std::filesystem::u8path(u8"工作区");
    std::filesystem::create_directory(workspace);
    const auto runtime_root = temp.path() / "runtime";
    // Keep the server alive through cold startup and the process deadline.
    test::ScriptedProvider provider({{test::RequestKind::Normal, u8"已记录"},
        {test::RequestKind::Normal, u8"蓝鲸"}});

    const auto runtime_utf8 = runtime_root.generic_u8string();
    temp.write_text(
        ".env",
        "AGENT_BASE_URL=http://127.0.0.1:" + std::to_string(provider.port()) + "\n"
        "AGENT_MODEL=MiniMax-M3\n"
        "AGENT_API_KEY=PROCESS_TEST_SECRET\n"
        "AGENT_RUNTIME_ROOT=" + runtime_utf8 + "\n"
        "AGENT_ENABLE_BUILD_TOOLS=0\n"
        "AGENT_ENABLE_RAG=0\n"
        "AGENT_ENABLE_MEMORY=0\n");
    const std::string first_question = u8"记住代号蓝鲸。只回答：已记录";
    const std::string second_question = u8"代号是什么？只回答代号。";
    agent::DirectProcessRunner process;
    const auto launched = process.run(
        {test::executable_under_test(),
         {"--env-file", (temp.path() / ".env").generic_u8string()},
         temp.path(),
         workspace.generic_u8string() + "\n" + first_question + "\n" +
             second_question + "\n/status\n/exit\n",
         30'000,
         256 * 1024,
         64 * 1024,
         {}});
    if (launched.has_value() && (launched.value().timed_out || !launched.value().stderr_utf8.empty()))
        std::cout << "two-turn process diagnostic: duration_ms=" << launched.value().duration_ms
                  << ", timeout=" << launched.value().timed_out
                  << ", exit_code=" << launched.value().exit_code << '\n';
    provider.verify();
    const auto& requests = provider.requests;

    REQUIRE(launched.has_value());
    REQUIRE(!launched.value().timed_out);
    REQUIRE(launched.value().exit_code == 0);
    REQUIRE(launched.value().stderr_utf8.empty());
    REQUIRE(requests.size() == 2);
    REQUIRE(launched.value().stdout_utf8.find(u8"已记录") !=
            std::string::npos);
    REQUIRE(launched.value().stdout_utf8.find(u8"蓝鲸") !=
            std::string::npos);
    REQUIRE(launched.value().stdout_utf8.find("Turns: 2") !=
            std::string::npos);
    REQUIRE(launched.value().stdout_utf8.find("Messages: 4") !=
            std::string::npos);

    const auto& first_messages = requests.at(0).at("messages");
    REQUIRE(first_messages.size() == 1);
    REQUIRE(first_messages.at(0).at("role") == "user");
    REQUIRE(test::text_at(first_messages.at(0)) == first_question);
    const auto& second_messages = requests.at(1).at("messages");
    REQUIRE(second_messages.size() == 3);
    REQUIRE(second_messages.at(0).at("role") == "user");
    REQUIRE(test::text_at(second_messages.at(0)) == first_question);
    REQUIRE(second_messages.at(1).at("role") == "assistant");
    REQUIRE(test::text_at(second_messages.at(1)) == u8"已记录");
    REQUIRE(second_messages.at(2).at("role") == "user");
    REQUIRE(test::text_at(second_messages.at(2)) == second_question);

    agent::JsonlSessionStore sessions(runtime_root);
    const auto listed = sessions.list_sessions();
    REQUIRE(listed.has_value());
    REQUIRE(listed.value().size() == 1);
    const auto& session = listed.value().front();
    auto expected_workspace = workspace.lexically_normal().generic_u8string();
#if defined(_WIN32)
    std::transform(expected_workspace.begin(), expected_workspace.end(),
                   expected_workspace.begin(), [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
#endif
    REQUIRE(session.workspace_utf8 == expected_workspace);
    REQUIRE(session.completed_turns == 2);
    REQUIRE(session.messages.size() == 4);
    const auto session_events = sessions.read_session(session.session_id);
    REQUIRE(session_events.has_value());
    REQUIRE(session_events.value().size() == 5);

    std::vector<std::string> task_ids;
    for (const auto& event : session_events.value()) {
        if (const auto* started =
                std::get_if<agent::SessionTurnStartedPayload>(&event.payload)) {
            task_ids.push_back(started->task_id);
        }
    }
    REQUIRE(task_ids.size() == 2);
    agent::JsonlEventStore tasks(runtime_root);
    std::string persisted = test::read_all(
        sessions.event_path(session.session_id).value());
    for (std::size_t index = 0; index < task_ids.size(); ++index) {
        const auto events = tasks.read_task(task_ids.at(index));
        REQUIRE(events.has_value());
        const auto state = agent::replay_events(events.value());
        REQUIRE(state.has_value());
        REQUIRE(state.value().status == agent::TaskStatus::Completed);
        const std::optional<agent::SessionTaskLink> expected_link{
            {session.session_id, index + 1}};
        REQUIRE(state.value().session_link == expected_link);
        const auto path = tasks.event_path(task_ids.at(index));
        REQUIRE(path.has_value());
        persisted += test::read_all(path.value());
    }
    REQUIRE(persisted.find("PROCESS_TEST_SECRET") == std::string::npos);
}

TEST_CASE(interactive_executable_wires_compaction_memory_prompt_and_exact_credential_protection) {
    for (const bool enabled : {true, false}) {
        test::ScopedTempDir temp("task-six-main-wiring");
        const auto runtime_root = temp.path() / "runtime";
        const auto workspace = temp.path().generic_u8string();
        const std::string session_id = "session-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        const std::string memory_id = "memory-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        const std::string timestamp = "2020-01-01T00:00:00.000Z";
        const std::string protected_value = "sixfixtureopaquevalue";
        agent::JsonlSessionStore sessions(runtime_root);
        REQUIRE(sessions.append({1, 1, session_id, timestamp, "corr-1",
            agent::SessionStartedPayload{workspace, "fixture-model"}}).has_value());
        for (std::uint64_t turn = 1; turn <= 3; ++turn) {
            const auto task_id = "task-" + std::string(31, '0') + std::to_string(turn);
            const auto question = "C++ old turn " + std::string(200, 'x');
            REQUIRE(sessions.append({1, 2 * turn, session_id, timestamp, "corr-start",
                agent::SessionTurnStartedPayload{turn, task_id, question}}).has_value());
            REQUIRE(sessions.append({1, 2 * turn + 1, session_id, timestamp, "corr-end",
                agent::SessionTurnCommittedPayload{turn, task_id,
                    {{agent::Role::User, {agent::TextBlock{question}}},
                     {agent::Role::Assistant, {agent::TextBlock{"answer"}}}}}}).has_value());
        }
        agent::JsonlMemoryStore memories(runtime_root);
        agent::MemoryEntry entry{memory_id, agent::MemoryCategory::Fact, workspace,
            "C++ tests are offline", session_id, 1, 3, timestamp, timestamp,
            agent::MemoryOrigin::ExplicitUser};
        REQUIRE(memories.append({1, 1, timestamp, "corr-memory",
            agent::MemoryUpsertedPayload{entry}}).has_value());
        REQUIRE(memories.append({1, 2, timestamp, "corr-checkpoint",
            agent::SessionMemoryConsolidatedPayload{session_id, 3}}).has_value());

        test::SocketRuntime sockets;
        std::uint16_t port = 0;
        const auto listener = test::create_listener(port);
        std::vector<nlohmann::json> requests;
        std::string server_error;
        std::thread server([&] {
            try {
                for (int index = 0; index < (enabled ? 3 : 2); ++index) {
                    if (test::wait_for_socket(listener, std::chrono::seconds(10)) != 1)
                        throw std::runtime_error("model request timed out");
                    const auto connection = accept(listener, nullptr, nullptr);
                    if (connection == test::kInvalidSocket)
                        throw std::runtime_error("model request accept failed");
                    requests.push_back(test::request_body(test::receive_request(connection)));
                    const auto response = test::provider_response("fixture-response",
                        index == 0 ? "C++ compacted summary" :
                        index == 1 ? "answer after compaction" : "{\"memories\":[]}");
                    const bool sent = test::send_all(connection, response);
                    test::close_socket(connection);
                    if (!sent) throw std::runtime_error("model response send failed");
                }
                test::close_socket(listener);
            } catch (...) {
                server_error = "loopback fixture failed";
                test::close_socket(listener);
            }
        });
        // Use synthetic config to avoid developer files.
        const auto config_file = temp.write_text("fixture-settings.cfg",
            "AGENT_BASE_URL=http://127.0.0.1:" + std::to_string(port) + "\n"
            "AGENT_MODEL=fixture-model\nAGENT_API_KEY=" + protected_value + "\n"
            "AGENT_RUNTIME_ROOT=" + runtime_root.generic_u8string() + "\n"
            "AGENT_ENABLE_RAG=0\nAGENT_ENABLE_BUILD_TOOLS=0\n"
            "AGENT_ENABLE_MEMORY=" + std::string(enabled ? "1" : "0") + "\n"
            "AGENT_SYSTEM_PROMPT=base sentinel\n"
            "AGENT_COMPACTION_THRESHOLD_BYTES=256\n"
            "AGENT_COMPACTION_HARD_LIMIT_BYTES=4096\n"
            "AGENT_COMPACTION_RETAIN_TURNS=1\n"
            "AGENT_COMPACTION_MAX_SUMMARY_BYTES=128\n");
        agent::DirectProcessRunner process;
        const auto launched = process.run({test::executable_under_test(),
            {"--env-file", config_file.generic_u8string()}, temp.path(),
            "/remember " + protected_value + "\nC++ question\n/status\n/exit\n",
            30'000, 256 * 1024, 64 * 1024, {}});
        server.join();
        REQUIRE(launched.has_value());
        REQUIRE(!launched.value().timed_out);
        REQUIRE(launched.value().exit_code == 0);
        const auto compacting = launched.value().stdout_utf8.find("Compacting context...");
        const auto compacted = launched.value().stdout_utf8.find("Context compacted.");
        REQUIRE(compacting != std::string::npos);
        REQUIRE(compacted != std::string::npos);
        REQUIRE(compacting < compacted);
        REQUIRE(server_error.empty());
        REQUIRE(requests.size() == (enabled ? 3U : 2U));
        const auto system = requests[1].at("system").get<std::string>();
        REQUIRE(system.find("base sentinel") != std::string::npos);
        REQUIRE(system.find("Session summary") != std::string::npos);
        REQUIRE(system.find("C++ compacted summary") != std::string::npos);
        REQUIRE((system.find("C++ tests are offline") != std::string::npos) == enabled);
        REQUIRE(requests[1].at("messages").size() == 3);
        REQUIRE((launched.value().stdout_utf8 + launched.value().stderr_utf8).find(protected_value) ==
                std::string::npos);
        for (const auto& request : requests)
            REQUIRE(request.dump().find(protected_value) == std::string::npos);
        const auto memory = memories.read_state();
        REQUIRE(memory.has_value());
        REQUIRE(memory.value().active_entries.size() == 1);
        REQUIRE(memory.value().session_checkpoints.at(session_id) == (enabled ? 4U : 3U));
        const auto session = sessions.list_sessions();
        REQUIRE(session.has_value());
        REQUIRE(session.value().front().compacted_through_turn == 2);
    }
}

TEST_CASE(interactive_executable_remembers_across_sessions_restarts_compacts_and_forgets) {
    // Verify prompts, persistence, exact tails, toggles, and checkpoints.
    using Kind = test::RequestKind;
    test::ScopedTempDir temp("memory-process-proof");
    const auto runtime = temp.path() / "runtime";
    agent::JsonlMemoryStore memories(runtime);
    agent::JsonlSessionStore sessions(runtime);
    const std::string summary = "Orion summary: the first cross-session question was handled.";
    const std::string first = "Orion first cross session " + std::string(180, 'x');
    const std::string retained = "Orion retained prior " + std::string(180, 'y');
    std::string memory_id;
    std::string source_session;
    std::string active_session;
    std::vector<nlohmann::json> all_requests;
    {
        test::ScriptedProvider provider({
            {Kind::Normal, "seed answer"}, {Kind::Consolidation, "{\"memories\":[]}"},
            {Kind::Normal, "first answer"}, {Kind::Normal, "retained answer"},
            {Kind::Compaction, summary}, {Kind::Normal, "compacted answer"},
            {Kind::Consolidation, "{\"memories\":[]}"}});
        const auto env = temp.write_text("first.cfg", test::fixture_config(runtime, provider.port(), true));
        const auto output = test::run_script(temp.path(), {"--env-file", env.generic_u8string()},
            "\nseed background\n/remember Orion review uses offline checks.\n/new\n" +
            first + "\n" + retained + "\nOrion after compaction\n/status\n/exit\n");
        provider.verify();
        const auto compacting = output.stdout_utf8.find("Compacting context...");
        const auto compacted = output.stdout_utf8.find("Context compacted.");
        REQUIRE(compacting != std::string::npos);
        REQUIRE(compacted != std::string::npos);
        REQUIRE(compacting < compacted);
        const auto memory = memories.read_state();
        REQUIRE(memory.has_value());
        REQUIRE(memory.value().active_entries.size() == 1);
        const auto& entry = memory.value().active_entries.begin()->second;
        memory_id = entry.memory_id;
        source_session = entry.source_session_id;
        REQUIRE(entry.origin == agent::MemoryOrigin::ExplicitUser);
        REQUIRE(entry.content == "Orion review uses offline checks.");
        REQUIRE(output.stdout_utf8.find(memory_id) != std::string::npos);
        const auto listed = sessions.list_sessions();
        REQUIRE(listed.has_value());
        REQUIRE(listed.value().size() == 2);
        for (const auto& session : listed.value()) {
            if (session.session_id == source_session) {
                REQUIRE(session.completed_turns == 1);
            } else {
                active_session = session.session_id;
                REQUIRE(session.completed_turns == 3);
                REQUIRE(session.compacted_through_turn == 1);
                REQUIRE(session.summary == summary);
                REQUIRE(session.messages.size() == 4);
            }
        }
        REQUIRE(!active_session.empty());
        REQUIRE(memory.value().session_checkpoints.at(source_session) == 1);
        REQUIRE(memory.value().session_checkpoints.at(active_session) == 3);
        for (const auto index : {2U, 3U, 5U})
            test::assert_memory_injection(provider.requests.at(index), memory_id, true);
        const auto compact_input = nlohmann::json::parse(test::text_at(provider.requests[4].at("messages")[0]));
        REQUIRE(compact_input.at("turns").size() == 1);
        REQUIRE(compact_input.at("turns")[0].at("turn_index") == 1);
        REQUIRE(test::text_at(compact_input.at("turns")[0].at("messages")[0]) == first);
        const auto& normal = provider.requests[5];
        REQUIRE(normal.at("system").get<std::string>().find("--- BEGIN Session summary ---") != std::string::npos);
        REQUIRE(normal.at("system").get<std::string>().find(summary) != std::string::npos);
        const auto& tail = normal.at("messages");
        REQUIRE(tail.size() == 3);
        REQUIRE(test::text_at(tail[0]) == retained);
        REQUIRE(test::text_at(tail[1]) == "retained answer");
        REQUIRE(test::text_at(tail[2]) == "Orion after compaction");
        REQUIRE(normal.dump().find(first) == std::string::npos);
        const auto boundary = nlohmann::json::parse(test::text_at(provider.requests[6].at("messages")[0]));
        REQUIRE(boundary.at("source_turn_start") == 1);
        REQUIRE(boundary.at("source_turn_end") == 3);
        REQUIRE(boundary.at("turns").size() == 3);
        REQUIRE(test::text_at(boundary.at("turns")[0].at("messages")[0]) == first);
        all_requests.insert(all_requests.end(), provider.requests.begin(), provider.requests.end());
    }
    {
        // Leave consolidation pending; the next process must catch up.
        test::ScriptedProvider provider({{Kind::Normal, "restart answer"},
            {Kind::Normal, "disabled answer"}, {Kind::Normal, "enabled answer"}});
        const auto env = temp.write_text("second.cfg", test::fixture_config(runtime, provider.port()));
        const auto output = test::run_script(temp.path(), {"--env-file", env.generic_u8string()},
            "Orion restart\n/memory off\nOrion disabled\n/memories\n/memory on\n"
            "Orion enabled\n/memory off\n/exit\n");
        provider.verify();
        test::assert_memory_injection(provider.requests[0], memory_id, true);
        REQUIRE(provider.requests[0].at("system").get<std::string>().find(summary) != std::string::npos);
        test::assert_memory_injection(provider.requests[1], memory_id, false);
        test::assert_memory_injection(provider.requests[2], memory_id, true);
        REQUIRE(output.stdout_utf8.find("Session: " + active_session) != std::string::npos);
        REQUIRE(output.stdout_utf8.find(memory_id + " | fact |") != std::string::npos);
        const auto memory = memories.read_state();
        REQUIRE(memory.has_value());
        REQUIRE(memory.value().active_entries.count(memory_id) == 1);
        REQUIRE(memory.value().session_checkpoints.at(active_session) == 3);
        all_requests.insert(all_requests.end(), provider.requests.begin(), provider.requests.end());
    }
    {
        test::ScriptedProvider provider({{Kind::Consolidation,
            "{\"memories\":[{\"category\":\"workflow\",\"scope\":\"workspace\","
            "\"content\":\"Build policy requires hermetic fixtures.\"}]}"},
            {Kind::Normal, "forgotten answer"}, {Kind::Consolidation, "{\"memories\":[]}"}});
        const auto env = temp.write_text("third.cfg", test::fixture_config(runtime, provider.port()));
        const auto output = test::run_script(temp.path(), {"--env-file", env.generic_u8string()},
            "/forget " + memory_id + "\nOrion after forgetting\n/exit\n");
        provider.verify();
        REQUIRE(output.stdout_utf8.find("Forgotten: " + memory_id) != std::string::npos);
        const auto catch_up = nlohmann::json::parse(test::text_at(provider.requests[0].at("messages")[0]));
        REQUIRE(catch_up.at("session_id") == active_session);
        REQUIRE(catch_up.at("source_turn_start") == 4);
        REQUIRE(catch_up.at("source_turn_end") == 6);
        REQUIRE(catch_up.at("turns").size() == 3);
        test::assert_memory_injection(provider.requests[1], memory_id, false);
        const auto memory = memories.read_state();
        REQUIRE(memory.has_value());
        REQUIRE(memory.value().active_entries.count(memory_id) == 0);
        REQUIRE(memory.value().used_memory_ids.count(memory_id) == 1);
        REQUIRE(memory.value().active_entries.size() == 1);
        REQUIRE(memory.value().active_entries.begin()->second.origin == agent::MemoryOrigin::ModelConsolidation);
        REQUIRE(memory.value().session_checkpoints.at(active_session) == 7);
        all_requests.insert(all_requests.end(), provider.requests.begin(), provider.requests.end());
    }
    {
        test::ScriptedProvider provider({});
        const auto env = temp.write_text("fourth.cfg", test::fixture_config(runtime, provider.port()));
        const auto before = memories.read_all();
        REQUIRE(before.has_value());
        test::run_script(temp.path(), {"--env-file", env.generic_u8string()}, "/status\n/exit\n");
        provider.verify();
        const auto after = memories.read_all();
        REQUIRE(after.has_value());
        REQUIRE(after.value() == before.value());
    }
    REQUIRE(all_requests.size() == 13);
    REQUIRE(std::count_if(all_requests.begin(), all_requests.end(), [](const auto& request) {
        return test::classify_request(request) == Kind::Normal;
    }) == 8);
    REQUIRE(std::count_if(all_requests.begin(), all_requests.end(), [](const auto& request) {
        return test::classify_request(request) == Kind::Compaction;
    }) == 1);
    REQUIRE(std::count_if(all_requests.begin(), all_requests.end(), [](const auto& request) {
        return test::classify_request(request) == Kind::Consolidation;
    }) == 4);
    const auto memory_events = memories.read_all();
    REQUIRE(memory_events.has_value());
    test::assert_sequences(memory_events.value());
    REQUIRE(memory_events.value().size() == 7);
    REQUIRE(std::get<agent::MemoryForgottenPayload>(memory_events.value()[5].payload).memory_id == memory_id);
    agent::JsonlEventStore tasks(runtime);
    std::size_t task_count = 0;
    std::size_t compaction_count = 0;
    const auto final_sessions = sessions.list_sessions();
    REQUIRE(final_sessions.has_value());
    for (const auto& session : final_sessions.value()) {
        const auto events = sessions.read_session(session.session_id);
        REQUIRE(events.has_value());
        test::assert_sequences(events.value());
        for (const auto& event : events.value()) {
            if (std::holds_alternative<agent::SessionCompactedPayload>(event.payload)) ++compaction_count;
            if (const auto* committed = std::get_if<agent::SessionTurnCommittedPayload>(&event.payload)) {
                const auto task_events = tasks.read_task(committed->task_id);
                REQUIRE(task_events.has_value());
                test::assert_sequences(task_events.value());
                const auto state = agent::replay_events(task_events.value());
                REQUIRE(state.has_value());
                REQUIRE(state.value().status == agent::TaskStatus::Completed);
                REQUIRE(state.value().session_link == std::optional<agent::SessionTaskLink>(
                    {session.session_id, committed->turn_index}));
                ++task_count;
            }
        }
    }
    REQUIRE(task_count == 8);
    REQUIRE(compaction_count == 1);
    for (const auto& file : std::filesystem::recursive_directory_iterator(runtime)) {
        if (file.is_regular_file() && file.path().extension() == ".jsonl")
            REQUIRE(test::read_all(file.path()).find(test::kSyntheticCredential) == std::string::npos);
    }
    for (const auto& request : all_requests) {
        REQUIRE(request.dump().find(test::kSyntheticCredential) == std::string::npos);
        if (test::classify_request(request) != Kind::Normal) {
            REQUIRE(!request.contains("tools") || request.at("tools").empty());
            REQUIRE(request.at("messages").size() == 1);
        }
    }
}

TEST_CASE(interactive_executable_discovers_colocated_env_from_another_working_directory) {
    // Require colocated config discovery and all runtime DLLs.
    test::ScopedTempDir temp("ready-colocated-process");
    const auto source_exe = std::filesystem::u8path(test::executable_under_test());
    const auto package_env = source_exe.parent_path() / ".env";
    if (std::getenv("AGENT_PROCESS_TEST_EXE") != nullptr) {
        // Check attributes without opening credentials.
        REQUIRE(std::filesystem::is_regular_file(package_env));
        REQUIRE(!std::filesystem::is_symlink(package_env));
        REQUIRE(std::filesystem::hard_link_count(package_env) == 1);
        REQUIRE(std::filesystem::file_size(package_env) > 0);
    }
    const auto deployment = temp.path() / "deployment";
    const auto cwd = temp.path() / "unrelated-working-directory";
    const auto runtime = temp.path() / "absolute-runtime";
    std::filesystem::create_directory(deployment);
    std::filesystem::create_directory(cwd);
    const auto executable = deployment / source_exe.filename();
    std::filesystem::copy_file(source_exe, executable);
    REQUIRE(test::read_all(source_exe) == test::read_all(executable));
    for (const auto& file : std::filesystem::directory_iterator(source_exe.parent_path())) {
        if (file.is_regular_file() && file.path().extension() == ".dll")
            std::filesystem::copy_file(file.path(), deployment / file.path().filename());
    }
    test::ScriptedProvider provider({{test::RequestKind::Normal, "colocated configuration works"},
        {test::RequestKind::Consolidation, "{\"memories\":[]}"}});
    temp.write_text("deployment/.env", test::fixture_config(runtime, provider.port()));
    const auto output = test::run_script(cwd, {}, "\ncolocated startup proof\n/exit\n", executable.generic_u8string());
    provider.verify();
    REQUIRE(output.stdout_utf8.find("colocated configuration works") != std::string::npos);
    REQUIRE(provider.requests[0].at("model") == "MiniMax-M3");
    REQUIRE(test::text_at(provider.requests[0].at("messages")[0]) == "colocated startup proof");
    REQUIRE(std::filesystem::is_directory(runtime / "sessions"));
    REQUIRE(!std::filesystem::exists(deployment / "runtime_data"));
    REQUIRE(!std::filesystem::exists(cwd / "runtime_data"));
}

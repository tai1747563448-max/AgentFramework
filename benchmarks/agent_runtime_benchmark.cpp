#include "application/benchmark_statistics.h"
#include "application/runtime_engine.h"
#include "application/task_evaluator.h"
#include "adapters/workspace/workspace_text.h"
#include "domain/model_types.h"
#include "domain/runtime_error.h"
#include "ports/cancellation.h"
#include "ports/clock.h"
#include "ports/event_store.h"
#include "ports/id_generator.h"
#include "ports/knowledge_provider.h"
#include "ports/model_client.h"
#include "ports/operation_context.h"
#include "ports/tool_gateway.h"
#include "benchmark_provenance.h"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifndef AGENT_BUILD_CONFIG
#define AGENT_BUILD_CONFIG "unknown"
#endif

namespace {

using Json = nlohmann::json;

constexpr int kSuccess = 0;
constexpr int kInvalidInput = 2;
constexpr int kRegression = 3;
constexpr int kReportSchemaVersion = 2;

struct Options {
    std::size_t warmup{5};
    std::size_t iterations{1'000};
    std::size_t batch_size{1'000};
    std::filesystem::path output;
    std::optional<std::filesystem::path> baseline;
    double max_regression_percent{15.0};
};

struct ScenarioResult {
    std::string name;
    agent::BenchmarkSummary summary;
};

struct LoadedReport {
    Json json;
    std::string sha256;
};

std::optional<std::size_t> json_size(const Json& value) {
    if (!value.is_number_integer()) {
        return std::nullopt;
    }
    try {
        if (value.is_number_unsigned()) {
            const auto parsed = value.get<std::uint64_t>();
            if (parsed > std::numeric_limits<std::size_t>::max()) {
                return std::nullopt;
            }
            return static_cast<std::size_t>(parsed);
        }
        const auto parsed = value.get<std::int64_t>();
        if (parsed < 0 ||
            static_cast<std::uint64_t>(parsed) >
                std::numeric_limits<std::size_t>::max()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(parsed);
    } catch (...) {
        return std::nullopt;
    }
}

class ScriptedModel final : public agent::ModelClient {
public:
    explicit ScriptedModel(std::vector<agent::ModelResponse> responses)
        : responses_(std::move(responses)) {}

    agent::Result<agent::ModelResponse> complete(
        const agent::ModelRequest&) override {
        if (next_ >= responses_.size()) {
            return agent::Result<agent::ModelResponse>::failure(
                {agent::ErrorCode::ProtocolFailure,
                 "benchmark response script exhausted", false});
        }
        return agent::Result<agent::ModelResponse>::success(
            responses_[next_++]);
    }

private:
    std::vector<agent::ModelResponse> responses_;
    std::size_t next_{0};
};

class NoopToolGateway final : public agent::ToolGateway {
public:
    std::vector<agent::ToolDefinition> definitions() const override {
        return {{"benchmark_noop", "Return a deterministic benchmark result.",
                 agent::Value::object({
                     {"type", "object"},
                     {"properties", agent::Value::object({})},
                     {"additionalProperties", false}})}};
    }

    agent::Result<agent::ToolResult> execute(
        const agent::ToolCall& call,
        const agent::ToolExecutionContext&) override {
        if (call.name != "benchmark_noop" || !call.arguments.is_object()) {
            return agent::Result<agent::ToolResult>::failure(
                {agent::ErrorCode::InvalidInput,
                 "invalid benchmark tool call", false});
        }
        return agent::Result<agent::ToolResult>::success(
            {call.id, "benchmark-ok", false});
    }
};

class EmptyKnowledge final : public agent::KnowledgeProvider {
public:
    agent::Result<agent::EvidencePack> retrieve(
        const agent::TaskState&,
        const agent::OperationContext&) override {
        return agent::Result<agent::EvidencePack>::success({});
    }
};

class InMemoryEventStore final : public agent::EventStore {
public:
    agent::Result<void> append(const agent::RuntimeEvent& event) override {
        events.push_back(event);
        return agent::Result<void>::success();
    }

    agent::Result<std::vector<agent::RuntimeEvent>> read_file(
        const std::filesystem::path&) const override {
        return agent::Result<std::vector<agent::RuntimeEvent>>::failure(
            {agent::ErrorCode::InvalidInput,
             "benchmark store has no file interface", false});
    }

    std::vector<agent::RuntimeEvent> events;
};

class FixedClock final : public agent::Clock {
public:
    std::string now_utc() const override {
        return "2026-09-03T00:00:00.000Z";
    }

    std::int64_t monotonic_ms() const override {
        return 1'000;
    }
};

class FixedIds final : public agent::IdGenerator {
public:
    std::string next_task_id() override {
        return "task-00000000000000000000000000000042";
    }

    std::string next_correlation_id() override {
        return "corr-benchmark-" + std::to_string(next_++);
    }

private:
    std::size_t next_{1};
};

class NeverCancelled final : public agent::Cancellation {
public:
    bool requested() const noexcept override {
        return false;
    }
};

agent::ModelResponse text_response(std::string request_id) {
    return {{agent::TextBlock{"benchmark-complete"}},
            agent::StopReason::EndTurn, 8, 2,
            std::move(request_id)};
}

agent::ModelResponse tool_response() {
    agent::ToolCall call{
        "call-benchmark-1", "benchmark_noop", agent::Value::object({})};
    return {{agent::ToolUseBlock{std::move(call)}},
            agent::StopReason::ToolUse, 8, 2,
            "benchmark-tool-request"};
}

agent::RunRequest run_request() {
    return {"Run the controlled benchmark scenario.", ".",
            "You are a deterministic benchmark provider.",
            agent::RuntimeBudgets{4, 4, 30'000, 5'000}};
}

bool run_runtime(std::vector<agent::ModelResponse> responses,
                 agent::ToolGateway& tools,
                 std::vector<agent::RuntimeEvent>* captured_events = nullptr) {
    ScriptedModel model(std::move(responses));
    EmptyKnowledge knowledge;
    InMemoryEventStore events;
    FixedClock clock;
    FixedIds ids;
    NeverCancelled cancellation;
    agent::RuntimeEngine engine(
        model, tools, knowledge, events, clock, ids, cancellation);
    const auto result = engine.run(run_request(), {});
    if (captured_events != nullptr) {
        *captured_events = events.events;
    }
    return result.state.has_value() && !result.fatal_error.has_value() &&
           result.state->status == agent::TaskStatus::Completed &&
           result.state->final_text ==
               std::optional<std::string>{"benchmark-complete"};
}

bool text_completion_operation() {
    NoopToolGateway tools;
    return run_runtime({text_response("benchmark-text-request")}, tools);
}

bool tool_round_trip_operation() {
    NoopToolGateway tools;
    return run_runtime(
        {tool_response(), text_response("benchmark-final-request")}, tools);
}

std::optional<std::vector<agent::RuntimeEvent>> completed_events() {
    NoopToolGateway tools;
    std::vector<agent::RuntimeEvent> events;
    if (!run_runtime({text_response("benchmark-event-fixture")}, tools,
                     &events)) {
        return std::nullopt;
    }
    return events;
}

bool parse_size(const std::string& text,
                bool allow_zero,
                std::size_t& value) {
    if (text.empty()) {
        return false;
    }
    std::size_t parsed = 0;
    const auto result = std::from_chars(
        text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        (!allow_zero && parsed == 0)) {
        return false;
    }
    value = parsed;
    return true;
}

bool parse_nonnegative_double(const std::string& text, double& value) {
    if (text.empty()) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (errno == ERANGE || end != text.c_str() + text.size() ||
        !std::isfinite(parsed) || parsed < 0.0) {
        return false;
    }
    value = parsed;
    return true;
}

std::optional<Options> parse_options(int argc, char* argv[]) {
    Options options;
    bool saw_warmup = false;
    bool saw_iterations = false;
    bool saw_batch_size = false;
    bool saw_output = false;
    bool saw_baseline = false;
    bool saw_regression = false;

    for (int index = 1; index < argc; index += 2) {
        if (index + 1 >= argc) {
            return std::nullopt;
        }
        const std::string name = argv[index];
        const std::string value = argv[index + 1];
        if (name == "--warmup" && !saw_warmup) {
            saw_warmup = true;
            if (!parse_size(value, true, options.warmup)) {
                return std::nullopt;
            }
        } else if (name == "--iterations" && !saw_iterations) {
            saw_iterations = true;
            if (!parse_size(value, false, options.iterations)) {
                return std::nullopt;
            }
        } else if (name == "--batch-size" && !saw_batch_size) {
            saw_batch_size = true;
            if (!parse_size(value, false, options.batch_size)) {
                return std::nullopt;
            }
        } else if (name == "--output" && !saw_output && !value.empty()) {
            saw_output = true;
            options.output = std::filesystem::u8path(value);
        } else if (name == "--baseline" && !saw_baseline && !value.empty()) {
            saw_baseline = true;
            options.baseline = std::filesystem::u8path(value);
        } else if (name == "--max-regression-percent" && !saw_regression) {
            saw_regression = true;
            if (!parse_nonnegative_double(
                    value, options.max_regression_percent)) {
                return std::nullopt;
            }
        } else {
            return std::nullopt;
        }
    }
    if (!saw_output) {
        return std::nullopt;
    }
    if (options.iterations >
        std::numeric_limits<std::size_t>::max() / options.batch_size) {
        return std::nullopt;
    }
    return options;
}

std::optional<ScenarioResult> run_scenario(
    std::string name,
    const std::function<bool()>& operation,
    std::size_t warmup,
    std::size_t iterations,
    std::size_t batch_size) {
    for (std::size_t index = 0; index < warmup; ++index) {
        for (std::size_t operation_index = 0;
             operation_index < batch_size; ++operation_index) {
            if (!operation()) {
                return std::nullopt;
            }
        }
    }

    std::vector<agent::BenchmarkObservation> observations;
    observations.reserve(iterations);
    for (std::size_t index = 0; index < iterations; ++index) {
        const auto started = std::chrono::steady_clock::now();
        std::size_t success_count = 0;
        for (std::size_t operation_index = 0;
             operation_index < batch_size; ++operation_index) {
            if (operation()) {
                ++success_count;
            }
        }
        const auto stopped = std::chrono::steady_clock::now();
        double duration_us =
            std::chrono::duration<double, std::micro>(stopped - started)
                .count();
        if (duration_us <= 0.0) {
            duration_us = std::numeric_limits<double>::epsilon();
        }
        observations.push_back({duration_us, batch_size, success_count});
    }

    auto summary = agent::summarize_benchmark(observations);
    if (!summary.has_value()) {
        return std::nullopt;
    }
    return ScenarioResult{std::move(name), std::move(summary.value())};
}

Json summary_json(const agent::BenchmarkSummary& summary) {
    return Json{{"sample_count", summary.sample_count},
                {"operation_count", summary.operation_count},
                {"success_count", summary.success_count},
                {"error_count", summary.error_count},
                {"total_duration_us", summary.total_duration_us},
                {"min_latency_us", summary.min_latency_us},
                {"mean_latency_us", summary.mean_latency_us},
                {"p50_latency_us", summary.p50_latency_us},
                {"p95_latency_us", summary.p95_latency_us},
                {"p99_latency_us", summary.p99_latency_us},
                {"max_latency_us", summary.max_latency_us},
                {"throughput_ops_per_second",
                 summary.throughput_ops_per_second},
                {"success_rate", summary.success_rate}};
}

std::optional<agent::BenchmarkSummary> summary_from_json(const Json& value) {
    if (!value.is_object()) {
        return std::nullopt;
    }
    try {
        const auto sample_count = json_size(value.at("sample_count"));
        const auto operation_count = json_size(value.at("operation_count"));
        const auto success_count = json_size(value.at("success_count"));
        const auto error_count = json_size(value.at("error_count"));
        if (!sample_count.has_value() || !operation_count.has_value() ||
            !success_count.has_value() || !error_count.has_value()) {
            return std::nullopt;
        }
        agent::BenchmarkSummary summary;
        summary.sample_count = *sample_count;
        summary.operation_count = *operation_count;
        summary.success_count = *success_count;
        summary.error_count = *error_count;
        summary.total_duration_us =
            value.at("total_duration_us").get<double>();
        summary.min_latency_us = value.at("min_latency_us").get<double>();
        summary.mean_latency_us = value.at("mean_latency_us").get<double>();
        summary.p50_latency_us = value.at("p50_latency_us").get<double>();
        summary.p95_latency_us = value.at("p95_latency_us").get<double>();
        summary.p99_latency_us = value.at("p99_latency_us").get<double>();
        summary.max_latency_us = value.at("max_latency_us").get<double>();
        summary.throughput_ops_per_second =
            value.at("throughput_ops_per_second").get<double>();
        summary.success_rate = value.at("success_rate").get<double>();

        const auto finite_positive = [](double number) {
            return std::isfinite(number) && number > 0.0;
        };
        const auto nearly_equal = [](double left, double right) {
            const double scale =
                std::max({1.0, std::abs(left), std::abs(right)});
            return std::abs(left - right) <= scale * 1e-9;
        };
        if (summary.sample_count == 0 || summary.operation_count == 0 ||
            summary.success_count > summary.operation_count ||
            summary.error_count !=
                summary.operation_count - summary.success_count ||
            !finite_positive(summary.total_duration_us) ||
            !finite_positive(summary.min_latency_us) ||
            !finite_positive(summary.mean_latency_us) ||
            !finite_positive(summary.p50_latency_us) ||
            !finite_positive(summary.p95_latency_us) ||
            !finite_positive(summary.p99_latency_us) ||
            !finite_positive(summary.max_latency_us) ||
            !finite_positive(summary.throughput_ops_per_second) ||
            !std::isfinite(summary.success_rate) ||
            summary.success_rate < 0.0 || summary.success_rate > 1.0 ||
            summary.min_latency_us > summary.mean_latency_us ||
            summary.mean_latency_us > summary.max_latency_us ||
            summary.min_latency_us > summary.p50_latency_us ||
            summary.p50_latency_us > summary.p95_latency_us ||
            summary.p95_latency_us > summary.p99_latency_us ||
            summary.p99_latency_us > summary.max_latency_us) {
            return std::nullopt;
        }
        const double expected_mean =
            summary.total_duration_us /
            static_cast<double>(summary.operation_count);
        const double expected_throughput =
            static_cast<double>(summary.operation_count) * 1'000'000.0 /
            summary.total_duration_us;
        const double expected_success_rate =
            static_cast<double>(summary.success_count) /
            static_cast<double>(summary.operation_count);
        if (!nearly_equal(summary.mean_latency_us, expected_mean) ||
            !nearly_equal(summary.throughput_ops_per_second,
                          expected_throughput) ||
            !nearly_equal(summary.success_rate, expected_success_rate)) {
            return std::nullopt;
        }
        return summary;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<LoadedReport> read_report(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::string bytes;
    bytes.assign(
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (input.bad()) {
        return std::nullopt;
    }
    Json parsed = Json::parse(bytes, nullptr, false);
    if (parsed.is_discarded()) {
        return std::nullopt;
    }
    return LoadedReport{
        std::move(parsed), agent::workspace::sha256_hex(bytes)};
}

std::optional<bool> paths_refer_to_same_file(
    const std::filesystem::path& left,
    const std::filesystem::path& right) {
    std::error_code error;
    const auto left_absolute = std::filesystem::absolute(left, error);
    if (error) {
        return std::nullopt;
    }
    const auto left_canonical =
        std::filesystem::weakly_canonical(left_absolute, error);
    if (error) {
        return std::nullopt;
    }
    const auto right_absolute = std::filesystem::absolute(right, error);
    if (error) {
        return std::nullopt;
    }
    const auto right_canonical =
        std::filesystem::weakly_canonical(right_absolute, error);
    if (error) {
        return std::nullopt;
    }
    if (left_canonical == right_canonical) {
        return true;
    }

    const bool left_exists = std::filesystem::exists(left_canonical, error);
    if (error) {
        return std::nullopt;
    }
    const bool right_exists = std::filesystem::exists(right_canonical, error);
    if (error) {
        return std::nullopt;
    }
    if (!left_exists || !right_exists) {
        return false;
    }
    const bool equivalent =
        std::filesystem::equivalent(left_canonical, right_canonical, error);
    return error ? std::nullopt : std::optional<bool>(equivalent);
}

std::optional<std::map<std::string, agent::BenchmarkSummary>>
validate_baseline_report(
    const Json& report,
    const Options& options,
    const Json& current_environment,
    const std::vector<ScenarioResult>& current_scenarios) {
    if (!report.is_object()) {
        return std::nullopt;
    }
    try {
        if (!report.at("schema_version").is_number_integer() ||
            report.at("schema_version").get<int>() != kReportSchemaVersion ||
            !report.at("benchmark_kind").is_string() ||
            report.at("benchmark_kind").get<std::string>() !=
                "controlled_offline_agent_runtime") {
            return std::nullopt;
        }

        const auto& parameters = report.at("parameters");
        if (!parameters.is_object()) {
            return std::nullopt;
        }
        const auto warmup = json_size(parameters.at("warmup"));
        const auto iterations = json_size(parameters.at("iterations"));
        const auto batch_size = json_size(parameters.at("batch_size"));
        const auto total_operations =
            json_size(parameters.at("total_operations"));
        if (!warmup.has_value() || !iterations.has_value() ||
            !batch_size.has_value() || !total_operations.has_value() ||
            *warmup != options.warmup || *iterations != options.iterations ||
            *batch_size != options.batch_size ||
            *total_operations != options.iterations * options.batch_size) {
            return std::nullopt;
        }

        const auto& methodology = report.at("methodology");
        if (!methodology.is_object() ||
            methodology.at("clock").get<std::string>() !=
                "std::chrono::steady_clock" ||
            !methodology.at("scripted_provider").get<bool>() ||
            methodology.at("external_network").get<bool>()) {
            return std::nullopt;
        }

        const auto& environment = report.at("environment");
        for (const char* key :
             {"operating_system", "compiler", "build_config"}) {
            if (!environment.at(key).is_string() ||
                environment.at(key) != current_environment.at(key)) {
                return std::nullopt;
            }
        }
        if (!environment.at("git_commit").is_string() ||
            environment.at("git_commit").get<std::string>().empty() ||
            !environment.at("working_tree_dirty").is_boolean()) {
            return std::nullopt;
        }

        const auto& scenarios = report.at("scenarios");
        if (!scenarios.is_array() ||
            scenarios.size() != current_scenarios.size()) {
            return std::nullopt;
        }
        std::map<std::string, agent::BenchmarkSummary> summaries;
        for (const auto& scenario : scenarios) {
            if (!scenario.is_object() || !scenario.at("name").is_string()) {
                return std::nullopt;
            }
            const auto summary = summary_from_json(scenario.at("summary"));
            if (!summary.has_value() ||
                summary->sample_count != *iterations ||
                summary->operation_count != *total_operations) {
                return std::nullopt;
            }
            const auto inserted = summaries.emplace(
                scenario.at("name").get<std::string>(), *summary);
            if (!inserted.second) {
                return std::nullopt;
            }
        }
        for (const auto& scenario : current_scenarios) {
            if (summaries.find(scenario.name) == summaries.end()) {
                return std::nullopt;
            }
        }
        return summaries;
    } catch (...) {
        return std::nullopt;
    }
}

bool write_report(const std::filesystem::path& path, const Json& report) {
    std::error_code error;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            return false;
        }
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output << std::setw(2) << report << '\n';
    output.flush();
    return static_cast<bool>(output);
}

Json environment_json() {
#if defined(_WIN32)
    const char* operating_system = "windows";
#elif defined(__APPLE__)
    const char* operating_system = "macos";
#elif defined(__linux__)
    const char* operating_system = "linux";
#else
    const char* operating_system = "unknown";
#endif

#if defined(_MSC_VER)
    const std::string compiler = "msvc-" + std::to_string(_MSC_VER);
#elif defined(__clang__)
    const std::string compiler = "clang-" +
                                 std::to_string(__clang_major__);
#elif defined(__GNUC__)
    const std::string compiler = "gcc-" +
                                 std::to_string(__GNUC__);
#else
    const std::string compiler = "unknown";
#endif
    return Json{{"operating_system", operating_system},
                {"compiler", compiler},
                {"build_config", AGENT_BUILD_CONFIG},
                {"git_commit", AGENT_GIT_COMMIT},
                {"working_tree_dirty", AGENT_GIT_DIRTY != 0}};
}

}  // namespace

int main(int argc, char* argv[]) {
    const auto options = parse_options(argc, argv);
    if (!options.has_value()) {
        std::cerr << "invalid benchmark arguments\n";
        return kInvalidInput;
    }
    if (options->baseline.has_value()) {
        const auto same_path =
            paths_refer_to_same_file(*options->baseline, options->output);
        if (!same_path.has_value() || *same_path) {
            std::cerr << "benchmark baseline and output must differ\n";
            return kInvalidInput;
        }
    }

    const auto fixture_events = completed_events();
    if (!fixture_events.has_value()) {
        std::cerr << "benchmark execution failed\n";
        return kRegression;
    }

    std::vector<ScenarioResult> scenarios;
    const std::vector<std::pair<std::string, std::function<bool()>>> operations{
        {"text_completion_runtime", text_completion_operation},
        {"tool_round_trip_runtime", tool_round_trip_operation},
        {"event_log_evaluation", [events = *fixture_events] {
             const auto evaluated = agent::evaluate_task_events(events);
             return evaluated.has_value() && evaluated.value().passed;
         }}};

    for (const auto& operation : operations) {
        auto result = run_scenario(
            operation.first, operation.second, options->warmup,
            options->iterations, options->batch_size);
        if (!result.has_value()) {
            std::cerr << "benchmark execution failed\n";
            return kRegression;
        }
        scenarios.push_back(std::move(*result));
    }

    const Json environment = environment_json();
    const Json methodology{{"clock", "std::chrono::steady_clock"},
                           {"scripted_provider", true},
                           {"external_network", false}};
    Json report{{"schema_version", kReportSchemaVersion},
                {"benchmark_kind", "controlled_offline_agent_runtime"},
                {"parameters",
                 {{"warmup", options->warmup},
                  {"iterations", options->iterations},
                  {"batch_size", options->batch_size},
                  {"total_operations",
                   options->iterations * options->batch_size}}},
                {"environment", environment},
                {"methodology", methodology},
                {"limitations",
                 Json::array({
                     "Measures deterministic Agent Runtime overhead, not model inference.",
                     "Does not measure GPU, provider-network, or production concurrency.",
                     "Compare reports only under the same build and machine conditions."})},
                {"scenarios", Json::array()}};

    bool all_operations_succeeded = true;
    for (const auto& scenario : scenarios) {
        report["scenarios"].push_back(
            {{"name", scenario.name}, {"summary", summary_json(scenario.summary)}});
        all_operations_succeeded =
            all_operations_succeeded &&
            scenario.summary.success_count == scenario.summary.operation_count;
        std::cout << std::fixed << std::setprecision(3)
                  << "scenario=" << scenario.name
                  << " samples=" << scenario.summary.sample_count
                  << " operations=" << scenario.summary.operation_count
                  << " success_rate=" << scenario.summary.success_rate
                  << " p50_us=" << scenario.summary.p50_latency_us
                  << " p95_us=" << scenario.summary.p95_latency_us
                  << " p99_us=" << scenario.summary.p99_latency_us
                  << " throughput_ops_per_second="
                  << scenario.summary.throughput_ops_per_second << '\n';
    }

    bool comparison_passed = true;
    Json comparison{{"requested", options->baseline.has_value()}};
    if (options->baseline.has_value()) {
        const auto baseline_report = read_report(*options->baseline);
        if (!baseline_report.has_value()) {
            std::cerr << "benchmark baseline is invalid\n";
            return kInvalidInput;
        }
        const auto baseline_summaries = validate_baseline_report(
            baseline_report->json, *options, environment, scenarios);
        if (!baseline_summaries.has_value()) {
            std::cerr << "benchmark baseline is invalid\n";
            return kInvalidInput;
        }
        comparison["compatibility_validated"] = true;
        comparison["baseline"] =
            {{"path", options->baseline->generic_u8string()},
             {"sha256", baseline_report->sha256},
             {"schema_version",
              baseline_report->json.at("schema_version")},
             {"git_commit",
              baseline_report->json.at("environment").at("git_commit")},
             {"working_tree_dirty",
              baseline_report->json.at("environment")
                  .at("working_tree_dirty")}};
        comparison["scenarios"] = Json::array();
        const agent::BenchmarkRegressionPolicy policy{
            options->max_regression_percent,
            options->max_regression_percent,
            1.0};
        for (const auto& scenario : scenarios) {
            const auto& baseline = baseline_summaries->at(scenario.name);
            const auto compared =
                agent::compare_benchmark(baseline, scenario.summary, policy);
            if (!compared.has_value()) {
                std::cerr << "benchmark baseline is invalid\n";
                return kInvalidInput;
            }
            comparison_passed = comparison_passed && compared.value().passed;
            comparison["scenarios"].push_back(
                {{"name", scenario.name},
                 {"passed", compared.value().passed},
                 {"p95_latency_change_percent",
                  compared.value().p95_latency_change_percent},
                 {"throughput_drop_percent",
                  compared.value().throughput_drop_percent},
                 {"latency_regressed", compared.value().latency_regressed},
                 {"throughput_regressed",
                  compared.value().throughput_regressed},
                 {"success_rate_below_minimum",
                  compared.value().success_rate_below_minimum}});
        }
        comparison["passed"] = comparison_passed;
        comparison["policy"] =
            {{"max_p95_latency_regression_percent",
              options->max_regression_percent},
             {"max_throughput_regression_percent",
              options->max_regression_percent},
             {"minimum_success_rate", 1.0}};
    }
    report["comparison"] = std::move(comparison);

    if (!write_report(options->output, report)) {
        std::cerr << "benchmark report could not be written\n";
        return kInvalidInput;
    }
    std::cout << "benchmark_report=" << options->output.generic_u8string()
              << " comparison="
              << (options->baseline.has_value()
                      ? (comparison_passed ? "pass" : "fail")
                      : "not_requested")
              << '\n';
    return all_operations_succeeded && comparison_passed
               ? kSuccess
               : kRegression;
}

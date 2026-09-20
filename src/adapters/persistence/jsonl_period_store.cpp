#include "adapters/persistence/jsonl_period_store.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <system_error>

namespace agent {
namespace {

using Json = nlohmann::json;

const char* state_name(PeriodState state) {
    switch (state) {
    case PeriodState::Open:   return "open";
    case PeriodState::Closed: return "closed";
    }
    return "unknown";
}

PeriodState state_from(const std::string& name) {
    if (name == "closed") return PeriodState::Closed;
    return PeriodState::Open;
}

Json period_to_json(const Period& p) {
    return Json{
        {"period_id", p.period_id},
        {"started_at_utc", p.started_at_utc},
        {"ended_at_utc", p.ended_at_utc},
        {"session_ids", p.session_ids},
        {"episodic_summary", p.episodic_summary},
        {"state", state_name(p.state)},
        {"sessions_count", p.sessions_count},
        {"tokens_in_period", p.tokens_in_period},
    };
}

Result<Period> period_from_json(const Json& json) {
    if (!json.is_object())
        return Result<Period>::failure(
            {ErrorCode::PersistenceFailure, "period record is not an object", false});
    try {
        Period p;
        p.period_id = json.at("period_id").get<std::string>();
        p.started_at_utc = json.at("started_at_utc").get<std::string>();
        p.ended_at_utc = json.at("ended_at_utc").get<std::string>();
        p.session_ids = json.at("session_ids").get<std::vector<std::string>>();
        p.episodic_summary = json.at("episodic_summary").get<std::string>();
        p.state = state_from(json.at("state").get<std::string>());
        p.sessions_count = json.at("sessions_count").get<std::uint64_t>();
        p.tokens_in_period = json.at("tokens_in_period").get<std::size_t>();
        return Result<Period>::success(std::move(p));
    } catch (const std::exception&) {
        return Result<Period>::failure(
            {ErrorCode::PersistenceFailure, "period record parse failed", false});
    }
}

}  // namespace

JsonlPeriodStore::JsonlPeriodStore(std::filesystem::path runtime_root)
    : runtime_root_(std::move(runtime_root)) {}

std::filesystem::path JsonlPeriodStore::snapshot_path() const {
    return runtime_root_ / "periods" / "snapshot.json";
}

Result<void> JsonlPeriodStore::write_all(const std::vector<Period>& periods) {
    std::error_code error;
    std::filesystem::create_directories(snapshot_path().parent_path(), error);
    if (error)
        return Result<void>::failure(
            {ErrorCode::PersistenceFailure, "cannot create periods dir", false});

    Json array = Json::array();
    for (const auto& p : periods) array.push_back(period_to_json(p));

    // Atomic write: dump to tmp file then rename. Avoids leaving a
    // half-written snapshot if the process dies mid-write.
    const auto tmp = snapshot_path().string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return Result<void>::failure(
                {ErrorCode::PersistenceFailure, "cannot open period tmp", false});
        out << array.dump();
        if (!out)
            return Result<void>::failure(
                {ErrorCode::PersistenceFailure, "period write failed", false});
    }
    std::filesystem::rename(tmp, snapshot_path(), error);
    if (error)
        return Result<void>::failure(
            {ErrorCode::PersistenceFailure, "period rename failed", false});
    return Result<void>::success();
}

Result<std::vector<Period>> JsonlPeriodStore::read_all() {
    std::error_code error;
    if (!std::filesystem::exists(snapshot_path(), error))
        return Result<std::vector<Period>>::success({});

    std::ifstream in(snapshot_path(), std::ios::binary);
    if (!in)
        return Result<std::vector<Period>>::failure(
            {ErrorCode::PersistenceFailure, "cannot open period snapshot", false});

    Json array;
    try {
        in >> array;
    } catch (const std::exception&) {
        return Result<std::vector<Period>>::failure(
            {ErrorCode::PersistenceFailure, "period snapshot parse failed", false});
    }
    if (!array.is_array())
        return Result<std::vector<Period>>::failure(
            {ErrorCode::PersistenceFailure, "period snapshot is not an array", false});

    std::vector<Period> out;
    out.reserve(array.size());
    for (const auto& item : array) {
        auto parsed = period_from_json(item);
        if (!parsed.has_value())
            return Result<std::vector<Period>>::failure(parsed.error());
        out.push_back(std::move(parsed).value());
    }
    return Result<std::vector<Period>>::success(std::move(out));
}

}  // namespace agent

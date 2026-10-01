#include <rr/library/mission_log.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <utility>

#include <rr/json/json.hpp>

namespace rr::library {

namespace {

std::int64_t unix_ms(std::chrono::system_clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}

// Empty strings are stored as NULL: "none", not "one with no name".
std::optional<std::string> or_null(const std::string& text) {
    return text.empty() ? std::nullopt : std::optional<std::string>{text};
}

std::string problems_json(const std::vector<std::string>& problems) {
    json::Json array = json::Json::array();
    for (const std::string& problem : problems) {
        array.push_back(problem);
    }
    return json::dump(array);
}

Result<agents::MissionRecord> mission_from(const db::Row& row) {
    agents::MissionRecord mission;
    mission.job_id = row.get_text(0);
    mission.mission = row.get_text(1);
    mission.kind = row.get_text(2);
    mission.prompt = row.get_optional_text(3).value_or("");
    mission.role = row.get_text(4);
    mission.started_at = std::chrono::system_clock::time_point{std::chrono::milliseconds{row.get_int(5)}};
    mission.duration = std::chrono::milliseconds{row.get_int(6)};
    if (const std::string status = row.get_text(7); status == "ok") {
        mission.verdict = agents::verdict_from_string(row.get_text(8));
    } else {
        mission.error =
            Error{errc_from_string(status).value_or(Errc::internal), row.get_optional_text(9).value_or("")};
    }
    if (const auto step = row.get_optional_text(10)) {
        mission.step = agents::step_from_string(*step);
    }
    mission.rungs = static_cast<int>(row.get_int(11));
    mission.answered_by = row.get_optional_text(12).value_or("");
    mission.samples = static_cast<int>(row.get_int(13));
    mission.accepted = static_cast<int>(row.get_int(14));
    mission.calls = static_cast<int>(row.get_int(15));
    mission.usage = llm::Usage{.prompt_tokens = row.get_int(16),
                               .completion_tokens = row.get_int(18),
                               .cached_prompt_tokens = row.get_int(17)};
    RR_TRY_ASSIGN(const json::Json problems, json::parse(row.get_text(19)));
    for (const json::Json& problem : problems) {
        mission.problems.push_back(problem.get<std::string>());
    }
    return mission;
}

}  // namespace

Result<void> MissionLog::record(const agents::MissionRecord& mission) {
    const std::string status =
        mission.error.has_value() ? std::string{to_string(mission.error->code())} : "ok";
    const std::optional<std::string> verdict =
        mission.verdict.has_value() ? std::optional{std::string{agents::to_string(*mission.verdict)}}
                                    : std::nullopt;
    const std::optional<std::string> error =
        mission.error.has_value() ? std::optional{mission.error->describe()} : std::nullopt;
    const std::optional<std::string> step = mission.step.has_value()
                                                ? std::optional{std::string{agents::to_string(*mission.step)}}
                                                : std::nullopt;

    return database_->write([&](db::WriteTx& tx) -> Result<void> {
        // A mission run again (a resumed job) replaces its row.
        RR_TRY(tx.execute(
            "INSERT INTO missions (job_id, mission, kind, prompt, role, started_at_ms, duration_ms, status, "
            "verdict, error, step, rungs, answered_by, samples, accepted, calls, prompt_tokens, "
            "cached_prompt_tokens, completion_tokens, problems) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, "
            "?10, "
            "?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19, ?20) "
            "ON CONFLICT (job_id, mission) DO UPDATE SET kind = excluded.kind, prompt = excluded.prompt, "
            "role = excluded.role, started_at_ms = excluded.started_at_ms, duration_ms = "
            "excluded.duration_ms, "
            "status = excluded.status, verdict = excluded.verdict, error = excluded.error, step = "
            "excluded.step, "
            "rungs = excluded.rungs, answered_by = excluded.answered_by, samples = excluded.samples, "
            "accepted = excluded.accepted, calls = excluded.calls, prompt_tokens = excluded.prompt_tokens, "
            "cached_prompt_tokens = excluded.cached_prompt_tokens, "
            "completion_tokens = excluded.completion_tokens, problems = excluded.problems",
            {mission.job_id,
             mission.mission,
             mission.kind,
             or_null(mission.prompt),
             mission.role,
             unix_ms(mission.started_at),
             mission.duration.count(),
             status,
             verdict,
             error,
             step,
             mission.rungs,
             or_null(mission.answered_by),
             mission.samples,
             mission.accepted,
             mission.calls,
             mission.usage.prompt_tokens,
             mission.usage.cached_prompt_tokens,
             mission.usage.completion_tokens,
             problems_json(mission.problems)}));
        return ok();
    });
}

Result<std::vector<agents::MissionRecord>> MissionLog::list(std::string_view job_id) const {
    return database_->read([&](db::ReadTx& tx) -> Result<std::vector<agents::MissionRecord>> {
        std::vector<agents::MissionRecord> missions;
        RR_TRY(tx.query(
            "SELECT job_id, mission, kind, prompt, role, started_at_ms, duration_ms, status, verdict, error, "
            "step, rungs, answered_by, samples, accepted, calls, prompt_tokens, cached_prompt_tokens, "
            "completion_tokens, problems FROM missions WHERE job_id = ?1 ORDER BY started_at_ms, mission",
            {job_id}, [&](const db::Row& row) -> Result<void> {
                RR_TRY_ASSIGN(agents::MissionRecord mission, mission_from(row));
                missions.push_back(std::move(mission));
                return ok();
            }));
        return missions;
    });
}

}  // namespace rr::library

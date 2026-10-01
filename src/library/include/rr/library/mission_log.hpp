// rr/library/mission_log.hpp — the battle log, kept in the library database.
//
// Why this exists
//   rr_agents' Army hands one MissionRecord per mission to a BattleLog
//   (rr/agents/army.hpp). MissionLog is the log that keeps them: one row per
//   mission in `missions` (migrations/0003_missions.sql), next to the calls
//   it made (llm_calls) and the job's other records. The UI shows a job's
//   battle from it, and the eval counts escalations, repairs and findings
//   per stage and per prompt version.
//
//   A mission run again, when a job resumes, replaces its row: the log says
//   how each mission ended last.
//
// Thread-safety: thread-safe (the database serializes writes).
#pragma once

#include <string_view>
#include <vector>

#include <rr/agents/army.hpp>
#include <rr/core/result.hpp>
#include <rr/db/database.hpp>

namespace rr::library {

class MissionLog final : public agents::BattleLog {
public:
    // `database` must have the library schema (open_database()) and outlive the log.
    explicit MissionLog(db::Database& database) : database_(&database) {}

    [[nodiscard]] Result<void> record(const agents::MissionRecord& mission) override;

    // A job's missions, in the order they started.
    [[nodiscard]] Result<std::vector<agents::MissionRecord>> list(std::string_view job_id) const;

private:
    db::Database* database_;
};

}  // namespace rr::library

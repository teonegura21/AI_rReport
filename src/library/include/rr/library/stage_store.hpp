// rr/library/stage_store.hpp — stage results: artifacts, the stage cache, and
// each job's runs of its stages (ADR-0004).
//
// Why this exists
//   A stage's result is an artifact: an immutable JSON document that follows
//   its schema, kept once under the BLAKE3 hash of its canonical text
//   (rr/json/json.hpp). The stage cache maps a stage's key to the artifact
//   that answered it. The key covers what the stage is, how it is set up, and
//   the hashes of its inputs (rr/pipeline/stage.hpp). A job's run of a stage
//   records the key and the artifact. commit() writes the three in one
//   transaction: a crash leaves all of them or none, so a job resumes exactly
//   where its last commit left it.
//
//   What a stage could not do, its findings, is kept with its cache entry: a
//   job that reuses the result reuses its findings too, and publishes them.
//
//   A result that depends on its job, not only on its inputs and setup, is
//   kept for that job alone: no cache entry, so no other job is served it.
//   That is a result a job's ceiling cut short (ADR-0018): a job allowed more
//   would do better.
//
// Thread-safety: thread-safe (the database serializes writes).
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/clock.hpp>
#include <rr/core/result.hpp>
#include <rr/db/database.hpp>
#include <rr/json/json.hpp>

namespace rr::library {

// What answers a stage's key.
struct CachedResult {
    std::string artifact{};               // the artifact's hash
    std::vector<std::string> findings{};  // what the stage could not do

    friend bool operator==(const CachedResult&, const CachedResult&) = default;
};

enum class StageStatus : std::uint8_t {
    done,
    failed,
};

[[nodiscard]] std::string_view to_string(StageStatus status) noexcept;

// A job's run of one stage, as recorded.
struct StageRun {
    std::string job_id{};
    std::string stage{};
    std::string key{};
    StageStatus status = StageStatus::done;
    bool cached = false;                    // the stage cache answered
    std::optional<std::string> artifact{};  // its hash, when done
    std::vector<std::string> findings{};    // what it could not do, when done
    std::optional<std::string> error{};     // why it failed
    std::chrono::system_clock::time_point started_at{};
    std::chrono::system_clock::time_point finished_at{};

    friend bool operator==(const StageRun&, const StageRun&) = default;
};

// A stage done, to commit.
struct StageResult {
    std::string job_id{};
    std::string stage{};
    int version = 0;
    std::string key{};
    std::string schema{};  // the artifact's schema: "artifacts/transcript"
    json::Json artifact{};
    std::vector<std::string> findings{};
    std::chrono::system_clock::time_point started_at{};
    // false: kept for this job alone, never served to another (see the
    // header comment).
    bool reusable = true;
};

class StageStore {
public:
    // `database` must have the library schema (open_database()); it and
    // `clock` must outlive this.
    StageStore(db::Database& database, const Clock& clock);

    // What answers `key`, if a stage has been done for it.
    [[nodiscard]] Result<std::optional<CachedResult>> cached(std::string_view key) const;

    // The artifact `hash`. Errc::not_found when there is none;
    // Errc::integrity when its body is not what its hash says.
    [[nodiscard]] Result<json::Json> artifact(std::string_view hash) const;

    // Commits a stage done: its artifact (kept once), its cache entry when
    // it is reusable, and the job's run, in one transaction. Returns the
    // artifact's hash.
    [[nodiscard]] Result<std::string> commit(const StageResult& result);

    // Records that the stage cache answered a job's stage.
    [[nodiscard]] Result<void> reuse(std::string_view job_id, std::string_view stage, std::string_view key,
                                     const CachedResult& cached,
                                     std::chrono::system_clock::time_point started_at);

    // Records that a job's stage failed, and why.
    [[nodiscard]] Result<void> record_failure(std::string_view job_id, std::string_view stage,
                                              std::string_view key, std::string_view error,
                                              std::chrono::system_clock::time_point started_at);

    // The runs of a job, in the order they started.
    [[nodiscard]] Result<std::vector<StageRun>> runs(std::string_view job_id) const;

private:
    db::Database* database_;
    const Clock* clock_;
};

}  // namespace rr::library

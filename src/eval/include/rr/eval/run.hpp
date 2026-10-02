// rr/eval/run.hpp — the eval's runs: the set's videos taken through the
// article stages at one tier, each a number of times, and their scores.
//
// Why this exists
//   A number about articles means something only next to how it was made.
//   A run fixes that: one set, one split, one tier, a number of replicates,
//   under a label that `rreport eval compare` and `gate` name it by
//   (ADR-0019). Each video of the split is taken through the stages once a
//   replicate, the replicate number its job's salt, so the replicates draw
//   different samples while a run done again draws the same (ADR-0020:
//   seeds derive from the salt). Its jobs are ordinary article jobs, made
//   from the videos' snapshots (rr/eval/snapshot.hpp): they share the stage
//   cache, the call log and the battle log, `rreport jobs` lists them and
//   `rreport report` pages them. What is the eval's own is which job is
//   which video and replicate of which run (migrations/0006_eval.sql), and
//   the scores of each.
//
//   A run is taken up where it stopped: the same label again keeps each job
//   done, resumes each job not done (its memo answers every call it made),
//   and makes the jobs still missing. A label never means two runs: asking
//   it of another set, split, tier or number of replicates is refused.
//
// Thread-safety: as db::Database: safe from many threads, one transaction
//   at a time.
#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/clock.hpp>
#include <rr/core/result.hpp>
#include <rr/core/ulid.hpp>
#include <rr/db/database.hpp>
#include <rr/eval/set.hpp>
#include <rr/library/jobs.hpp>

namespace rr::eval {

// What a run measures.
struct RunSpec {
    std::string label{};     // what compare and gate name it by
    std::string eval_set{};  // the set's name: "m2"
    Split split = Split::dev;
    std::string tier{};  // fast | thorough | max
    int replicates = 1;  // at least 1

    friend bool operator==(const RunSpec&, const RunSpec&) = default;
};

struct EvalRun {
    std::string id{};  // a ULID
    RunSpec spec{};
    std::chrono::system_clock::time_point created_at{};

    friend bool operator==(const EvalRun&, const EvalRun&) = default;
};

// One video of a run, one replicate, and the job that takes it through.
struct RunEntry {
    std::string video{};  // the set's id for it
    int replicate = 0;    // from 0, and the job's salt
    std::string job{};

    friend bool operator==(const RunEntry&, const RunEntry&) = default;
};

// A metric's value for one entry.
struct Score {
    std::string metric{};  // "atoms_told", "minutes", …
    double value = 0;

    friend bool operator==(const Score&, const Score&) = default;
};

// An entry and its scores, as kept.
struct ScoredEntry {
    RunEntry entry{};
    std::string scorer{};  // what computed them: "code@1"
    std::vector<Score> scores{};

    friend bool operator==(const ScoredEntry&, const ScoredEntry&) = default;
};

class Runs {
public:
    // `database` must have the library schema (library::open_database());
    // it and `clock` must outlive this.
    Runs(db::Database& database, const Clock& clock);

    // The run `spec.label` names: made the first time it is asked for, the
    // same run after. Errors: Errc::invalid_argument for an empty label or
    // fewer than one replicate; Errc::conflict when the label names a run of
    // another set, split, tier or number of replicates.
    [[nodiscard]] Result<EvalRun> open(const RunSpec& spec);

    // The run `label` names. Errc::not_found when there is none.
    [[nodiscard]] Result<EvalRun> find(std::string_view label) const;

    // The entries of the run `run_id`, by video then replicate.
    [[nodiscard]] Result<std::vector<RunEntry>> entries(std::string_view run_id) const;

    // Records that `entry.job` takes `entry.video`'s replicate through.
    // Errc::conflict when that replicate has a job already.
    [[nodiscard]] Result<void> add(std::string_view run_id, const RunEntry& entry);

    // Keeps `scores` as `entry`'s, computed by `scorer`, replacing every
    // score it had: a job scored again is scored whole.
    [[nodiscard]] Result<void> keep(std::string_view run_id, const RunEntry& entry, std::string_view scorer,
                                    std::span<const Score> scores);

    // Every entry of the run with its scores, by video then replicate; an
    // entry not scored yet has none.
    [[nodiscard]] Result<std::vector<ScoredEntry>> scores(std::string_view run_id) const;

private:
    db::Database* database_;
    const Clock* clock_;
    std::unique_ptr<UlidGenerator> ids_;  // a pointer: the generator holds a mutex, and Runs moves
};

// What `run` takes through, in order: each of `videos` of the run's split
// that has a snapshot, once a replicate, replicate by replicate, so a run
// interrupted early has every video once before any twice; within one, in
// `videos`' order. An entry's `job` is its own where it has one, and empty
// where none was made yet (start_entry() makes it, when its turn comes).
struct Planned {
    std::vector<RunEntry> entries{};
    std::vector<std::string> unfetched{};  // videos without a snapshot: `rreport eval fetch` first
};
[[nodiscard]] Result<Planned> plan_run(const std::filesystem::path& eval_root, const EvalRun& run,
                                       std::span<const EvalVideo> videos, const Runs& runs);

// The job of `entry`: its own where it has one; else an article job made
// from its video's snapshot (rr/eval/snapshot.hpp), at the run's tier, its
// salt the replicate, recorded as the entry's, and `entry.job` set to it.
[[nodiscard]] Result<library::Job> start_entry(const std::filesystem::path& eval_root, const EvalRun& run,
                                               RunEntry& entry, Runs& runs, library::Jobs& jobs);

}  // namespace rr::eval

// The eval's runs (rr/eval/run.hpp).
#include <rr/eval/run.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <rr/core/error.hpp>
#include <rr/eval/snapshot.hpp>

namespace rr::eval {

namespace {

std::int64_t millis(std::chrono::system_clock::time_point at) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count();
}

Result<Split> split_from(std::string_view name) {
    if (name == to_string(Split::dev)) {
        return Split::dev;
    }
    if (name == to_string(Split::test)) {
        return Split::test;
    }
    return fail(Errc::integrity, std::format("an eval run of the split '{}', which is none", name));
}

constexpr std::string_view kRunColumns =
    "id, label, eval_set, split, tier, replicates, created_at_ms, first_replicate";

Result<EvalRun> run_of(const db::Row& row) {
    EvalRun run{
        .id = row.get_text(0),
        .spec = RunSpec{.label = row.get_text(1),
                        .eval_set = row.get_text(2),
                        .tier = row.get_text(4),
                        .replicates = static_cast<int>(row.get_int(5)),
                        .first_replicate = static_cast<int>(row.get_int(7))},
        .created_at = std::chrono::system_clock::time_point{std::chrono::milliseconds{row.get_int(6)}}};
    RR_TRY_ASSIGN(run.spec.split, split_from(row.get_text(3)));
    return run;
}

// The run `label` names, in `tx`.
template <class Tx>
Result<std::optional<EvalRun>> run_named(Tx& tx, std::string_view label) {
    std::optional<EvalRun> found;
    RR_TRY(tx.query(std::format("SELECT {} FROM eval_runs WHERE label = ?1", kRunColumns), {label},
                    [&](const db::Row& row) -> Result<void> {
                        RR_TRY_ASSIGN(found, run_of(row));
                        return ok();
                    }));
    return found;
}

// How `asked` differs from `run`'s spec, for the refusal; empty when it does not.
std::string differences(const RunSpec& asked, const RunSpec& run) {
    std::string said;
    const auto differ = [&](std::string_view what, std::string_view was, std::string_view now) {
        said += std::format("{}{} {}, not {}", said.empty() ? "" : "; ", what, was, now);
    };
    if (asked.eval_set != run.eval_set) {
        differ("the set", run.eval_set, asked.eval_set);
    }
    if (asked.split != run.split) {
        differ("the split", to_string(run.split), to_string(asked.split));
    }
    if (asked.tier != run.tier) {
        differ("the tier", run.tier, asked.tier);
    }
    if (asked.replicates != run.replicates) {
        differ("replicates", std::to_string(run.replicates), std::to_string(asked.replicates));
    }
    if (asked.first_replicate != run.first_replicate) {
        differ("the first replicate", std::to_string(run.first_replicate),
               std::to_string(asked.first_replicate));
    }
    return said;
}

}  // namespace

Runs::Runs(db::Database& database, const Clock& clock)
    : database_(&database)
    , clock_(&clock)
    , ids_(std::make_unique<UlidGenerator>(clock)) {}

Result<EvalRun> Runs::open(const RunSpec& spec) {
    if (spec.label.empty()) {
        return fail(Errc::invalid_argument, "an eval run needs a label: --label names it");
    }
    if (spec.replicates < 1) {
        return fail(Errc::invalid_argument,
                    std::format("an eval run takes each video once at least, not {} times", spec.replicates));
    }
    if (spec.first_replicate < 0) {
        return fail(Errc::invalid_argument,
                    std::format("replicates are numbered from 0, not {}", spec.first_replicate));
    }
    return database_->write([&](db::WriteTx& tx) -> Result<EvalRun> {
        RR_TRY_ASSIGN(const std::optional<EvalRun> existing, run_named(tx, spec.label));
        if (existing.has_value()) {
            if (const std::string said = differences(spec, existing->spec); !said.empty()) {
                return fail(Errc::conflict,
                            std::format("the run '{}' is of {}: a label names one run; choose another label",
                                        spec.label, said));
            }
            return *existing;
        }
        const auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(clock_->now());
        EvalRun run{.id = ids_->next(), .spec = spec, .created_at = now};
        RR_TRY(tx.execute(
            std::format("INSERT INTO eval_runs ({}) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)", kRunColumns),
            {run.id, spec.label, spec.eval_set, to_string(spec.split), spec.tier, spec.replicates,
             millis(now), spec.first_replicate}));
        return run;
    });
}

Result<EvalRun> Runs::find(std::string_view label) const {
    return database_->read([&](db::ReadTx& tx) -> Result<EvalRun> {
        RR_TRY_ASSIGN(std::optional<EvalRun> found, run_named(tx, label));
        if (!found.has_value()) {
            return fail(Errc::not_found,
                        std::format("no eval run is labelled '{}': `rreport eval run --label {}` makes it",
                                    label, label));
        }
        return *std::move(found);
    });
}

Result<std::vector<RunEntry>> Runs::entries(std::string_view run_id) const {
    return database_->read([&](db::ReadTx& tx) -> Result<std::vector<RunEntry>> {
        std::vector<RunEntry> entries;
        RR_TRY(
            tx.query("SELECT video, replicate, job_id FROM eval_jobs WHERE run_id = ?1 "
                     "ORDER BY video, replicate",
                     {run_id}, [&](const db::Row& row) -> Result<void> {
                         entries.push_back(RunEntry{.video = row.get_text(0),
                                                    .replicate = static_cast<int>(row.get_int(1)),
                                                    .job = row.get_text(2)});
                         return ok();
                     }));
        return entries;
    });
}

Result<void> Runs::add(std::string_view run_id, const RunEntry& entry) {
    return database_->write([&](db::WriteTx& tx) -> Result<void> {
        RR_TRY(tx.execute("INSERT INTO eval_jobs (run_id, video, replicate, job_id) VALUES (?1, ?2, ?3, ?4)",
                          {run_id, entry.video, entry.replicate, entry.job}));
        return ok();
    });
}

std::optional<std::string_view> scorer_family(std::string_view scorer) noexcept {
    const std::size_t at = scorer.find('@');
    if (at == 0 || at == std::string_view::npos || at + 1 == scorer.size()) {
        return std::nullopt;
    }
    return scorer.substr(0, at);
}

Result<void> Runs::keep(std::string_view run_id, const RunEntry& entry, const Scoring& scoring) {
    const std::optional<std::string_view> family = scorer_family(scoring.scorer);
    if (!family.has_value()) {
        return fail(Errc::invalid_argument,
                    std::format("a scorer is named family@version, as code@1 is: not '{}'", scoring.scorer));
    }
    const std::string prefix = std::format("{}@", *family);
    return database_->write([&](db::WriteTx& tx) -> Result<void> {
        RR_TRY(
            tx.execute("DELETE FROM eval_scores WHERE run_id = ?1 AND video = ?2 AND replicate = ?3 "
                       "AND substr(scorer, 1, length(?4)) = ?4",
                       {run_id, entry.video, entry.replicate, prefix}));
        for (const Score& score : scoring.scores) {
            RR_TRY(tx.execute(
                "INSERT INTO eval_scores (run_id, video, replicate, metric, value, scorer) "
                "VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
                {run_id, entry.video, entry.replicate, score.metric, score.value, scoring.scorer}));
        }
        return ok();
    });
}

Result<std::vector<ScoredEntry>> Runs::scores(std::string_view run_id) const {
    RR_TRY_ASSIGN(const std::vector<RunEntry> entries, this->entries(run_id));
    std::map<std::pair<std::string, int>, ScoredEntry> scored;
    for (const RunEntry& entry : entries) {
        scored.emplace(std::pair{entry.video, entry.replicate}, ScoredEntry{.entry = entry});
    }
    RR_TRY(database_->read([&](db::ReadTx& tx) {
        return tx.query(
            "SELECT video, replicate, scorer, metric, value FROM eval_scores WHERE run_id = ?1 "
            "ORDER BY video, replicate, scorer, metric",
            {run_id}, [&](const db::Row& row) -> Result<void> {
                const auto found = scored.find(std::pair{row.get_text(0), static_cast<int>(row.get_int(1))});
                if (found == scored.end()) {
                    return fail(Errc::integrity, "a score of no entry of its run");
                }
                std::vector<Scoring>& scorings = found->second.scorings;
                if (std::string scorer = row.get_text(2);
                    scorings.empty() || scorings.back().scorer != scorer) {
                    scorings.push_back(Scoring{.scorer = std::move(scorer)});
                }
                scorings.back().scores.push_back(
                    Score{.metric = row.get_text(3), .value = row.get_double(4)});
                return ok();
            });
    }));
    std::vector<ScoredEntry> all;
    all.reserve(scored.size());
    for (auto& [key, entry] : scored) {
        all.push_back(std::move(entry));
    }
    return all;
}

Result<Planned> plan_run(const std::filesystem::path& eval_root, const EvalRun& run,
                         std::span<const EvalVideo> videos, const Runs& runs) {
    RR_TRY_ASSIGN(const std::vector<RunEntry> made, runs.entries(run.id));
    std::map<std::pair<std::string, int>, std::string> jobs;
    for (const RunEntry& entry : made) {
        jobs.emplace(std::pair{entry.video, entry.replicate}, entry.job);
    }
    Planned planned;
    std::vector<const EvalVideo*> fetched;
    for (const EvalVideo& video : videos) {
        if (video.split != run.spec.split) {
            continue;
        }
        RR_TRY_ASSIGN(const std::optional<Snapshot> snapshot, read_snapshot(eval_root, video));
        if (snapshot.has_value()) {
            fetched.push_back(&video);
        } else {
            planned.unfetched.push_back(video.id);
        }
    }
    const int last = run.spec.first_replicate + run.spec.replicates;
    for (int replicate = run.spec.first_replicate; replicate < last; ++replicate) {
        for (const EvalVideo* const video : fetched) {
            const auto job = jobs.find(std::pair{video->id, replicate});
            planned.entries.push_back(RunEntry{.video = video->id,
                                               .replicate = replicate,
                                               .job = job == jobs.end() ? std::string{} : job->second});
        }
    }
    return planned;
}

Result<library::Job> start_entry(const std::filesystem::path& eval_root, const EvalRun& run, RunEntry& entry,
                                 Runs& runs, library::Jobs& jobs) {
    if (!entry.job.empty()) {
        return jobs.find(entry.job);
    }
    const std::string source = std::filesystem::absolute(snapshot_file(eval_root, entry.video)).string();
    RR_TRY_ASSIGN(library::Job job, jobs.create(source, run.spec.tier, entry.replicate));
    entry.job = job.id;
    RR_TRY(runs.add(run.id, entry));
    return job;
}

}  // namespace rr::eval

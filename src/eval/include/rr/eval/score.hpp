// rr/eval/score.hpp — an article measured by code alone: what it kept of its
// video, against the video's gold where a person reviewed it, and what it
// cost.
//
// Why this exists
//   ADR-0019 measures articles on three axes: the invariants a faithful
//   article never breaks, how much of what the video says it tells, and what
//   it cost. Code measures all it can; judges measure the rest (plan P24),
//   and stay report-only until a person has calibrated them (P27). So every
//   number here is exact and repeatable: the same artifacts score the same,
//   whenever they are scored.
//
// The metrics, each one job's (docs/design/eval.md has them as a table):
//   What the Fidelity Report shows, by the same code (report::checked()),
//   so the page and the eval never disagree:
//     sentences_accounted  Ledger A: sentences given atoms or a filler label
//     atoms_told           Ledger B: atoms the article cites
//     invalid_markers      markers that name no atom (a count, not a share)
//     quotes_kept          the atoms' words to quote, verbatim where cited
//     numbers_kept         the atoms' numbers, where cited
//   Against gold a person reviewed (rr/eval/gold.hpp), never silver:
//     gold_quotes_kept     must-quotes said verbatim in the article
//     gold_terms_used      terms the article uses
//     facts_cited          facts some atom of whose sentences the article
//     core_facts_cited     cites: those it may tell. Whether it tells them is
//                          a judge's to say (key-fact recall, P24); a fact
//                          not cited is not told, so these bound it
//   Form:
//     length_ratio         the article's words over the transcript's
//   Cost, the job's across its resumes:
//     minutes              its running time: on a pod of its own, its GPU's
//     minutes_per_video_hour
//     calls                model calls answered
//     prompt_tokens, completion_tokens   every attempt's
//     escalations          missions answered past the rung they were ordered at
//   A share with nothing to share (no number to keep, no term in the gold)
//   is no score at all, rather than a 100% that would flatter an average.
//
// Thread-safety: pure functions, but for costs_of() and score_run(), which
//   read and write the library (as db::Database).
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/clock.hpp>
#include <rr/core/result.hpp>
#include <rr/core/time_span.hpp>
#include <rr/db/database.hpp>
#include <rr/eval/gold.hpp>
#include <rr/eval/run.hpp>
#include <rr/eval/set.hpp>
#include <rr/knowledge/ids.hpp>
#include <rr/knowledge/transcript.hpp>
#include <rr/library/jobs.hpp>
#include <rr/report/page.hpp>

namespace rr::eval {

// What computes these scores, and its version: a change to any metric's
// definition bumps it, so scores of different definitions are never mixed.
inline constexpr std::string_view kCodeScorer = "code@1";

// What a job cost, across its resumes.
struct Costs {
    std::chrono::milliseconds elapsed{};
    std::int64_t calls = 0;              // answered
    std::int64_t prompt_tokens = 0;      // every attempt's, answered or not
    std::int64_t completion_tokens = 0;  // the same
    std::int64_t escalations = 0;        // missions answered past the rung they were ordered at

    friend bool operator==(const Costs&, const Costs&) = default;
};

// What `job` cost: its running time, its call log (library::CallLog) and its
// battle log (library::MissionLog).
[[nodiscard]] Result<Costs> costs_of(db::Database& database, const library::Job& job);

// The sentences of `transcript` that say `quote`: of those said within
// [start, end] (give or take kAnchorSlack, as said_there() reads them), the
// ones its verbatim alignment covers. None when it is not said there.
[[nodiscard]] std::vector<knowledge::SentenceId> sentences_saying(std::string_view quote, Millis start,
                                                                  Millis end,
                                                                  const knowledge::Transcript& transcript);

// The scores of `contents`, a job done, its article written: against `gold`
// where it is not null (reviewed gold of the snapshot the job read), and with
// `costs`. In the order of the header comment; none without an article.
[[nodiscard]] std::vector<Score> scores_of(const report::Contents& contents, const Gold* gold,
                                           const Costs& costs);

// What scoring a run did.
struct RunScoring {
    std::size_t scored = 0;              // entries scored now
    std::vector<std::string> waiting{};  // entries whose job is not done: "radium-talk, replicate 1: …"
    std::vector<std::string> notes{};    // what was scored without gold, and why

    friend bool operator==(const RunScoring&, const RunScoring&) = default;
};

// Scores each entry of `run` whose job is done (scores_of()) against its
// video's gold where a person reviewed it for the snapshot the job read, and
// keeps the scores (Runs::keep()), replacing what they were.
[[nodiscard]] Result<RunScoring> score_run(const std::filesystem::path& eval_root, const EvalSet& set,
                                           const EvalRun& run, Runs& runs, db::Database& database,
                                           const Clock& clock);

}  // namespace rr::eval

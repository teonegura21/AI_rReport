// rr/eval/export.hpp — a run's scores, for a spreadsheet or for the
// statistics.
//
// Why this exists
//   compare and gate (plan P25) read scores from the library. A person reads
//   them in a spreadsheet, and the report (P26) draws them. Both want one row
//   per video and replicate: the run's label and tier; the video's type and
//   caption kind, which the eval's breakdowns group by; its job, to open its
//   page; then each metric (rr/eval/score.hpp).
//
// Thread-safety: pure functions.
#pragma once

#include <span>
#include <string>

#include <rr/eval/run.hpp>
#include <rr/eval/set.hpp>
#include <rr/json/json.hpp>

namespace rr::eval {

// `entries`, the scores of `run`, as CSV (RFC 4180): a header, then a row an
// entry, its metrics in columns sorted by name; a metric an entry has not is
// an empty cell. A video no longer in `set` has no type nor caption kind.
[[nodiscard]] std::string to_csv(const EvalRun& run, const EvalSet& set,
                                 std::span<const ScoredEntry> entries);

// The same as JSON: {"run": {"label", "set", "split", "tier", "replicates"},
// "entries": [{"video", "replicate", "job", "type", "captions", "scorer",
// "scores": {metric: value, …}}, …]}.
[[nodiscard]] json::Json to_json(const EvalRun& run, const EvalSet& set,
                                 std::span<const ScoredEntry> entries);

}  // namespace rr::eval

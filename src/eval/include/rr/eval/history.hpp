// rr/eval/history.hpp — every eval run of a library on one page: what
// `rreport eval report` writes.
//
// Why this exists
//   A comparison answers one question about two runs (rr/eval/compare.hpp).
//   Choosing a default (runbook R6) needs the whole field at a glance: each
//   run's quality against what it cost, how sure each number is, and each
//   video's numbers under it. ADR-0018 puts quality first and measures cost
//   beside it; the chart puts the two on one plane, so that a variant that
//   buys little quality for much GPU time stands out, and one that loses
//   nothing for less is found.
//
// The page, one self-contained HTML file (nothing fetched):
//   1. The chart: an inline SVG, quality against cost. Each run a point at
//      its mean over videos (each video's replicates averaged first), with
//      bars spanning the 95% interval of each mean, resampled over the
//      videos as a comparison resamples them; labelled by the run. Runs
//      missing either metric are listed under it, not plotted.
//   2. The runs, newest first: what each is, how far it got, and the means
//      of a few headline metrics.
//   3. Each run's videos, folded: a row a video, a column a metric.
//
// Safety: labels, video ids and tier names are written by people; every
// text is escaped (report::escaped()), in elements and attributes alike.
//
// Thread-safety: pure functions.
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rr/eval/run.hpp>
#include <rr/eval/stats.hpp>

namespace rr::eval {

// A run and its entries' scores.
struct RunScores {
    EvalRun run{};
    std::vector<ScoredEntry> scores{};
};

// A run's mean of one metric over its videos, each video's replicates
// averaged first, and the 95% interval of that mean, resampled over the
// videos under the metric's seed (as compare() resamples differences).
struct RunMean {
    double mean = 0;
    std::optional<Interval> interval{};  // none for fewer than two videos
    std::size_t videos = 0;

    friend bool operator==(const RunMean&, const RunMean&) = default;
};

// None when no video of the run has `metric`.
[[nodiscard]] std::optional<RunMean> run_mean(std::span<const ScoredEntry> scores, std::string_view metric);

// A chart's axis: its bounds, and ticks a round step apart (1, 2 or 5 times
// a power of ten), the bounds among them.
struct Axis {
    double low = 0;
    double high = 1;
    double step = 0.2;
    std::vector<double> ticks{};

    friend bool operator==(const Axis&, const Axis&) = default;
};

// An axis holding every one of `values`, about five steps long; one value,
// or several alike, is given room around it. `values` is not empty.
[[nodiscard]] Axis axis_over(std::span<const double> values);

struct PageOptions {
    std::string quality{};                       // up the chart: a metric runs score
    std::string cost{"minutes_per_video_hour"};  // its width
    std::string made{};                          // when the page was made, as shown on it
};

// The quality the chart shows when none is asked for: the key facts a
// reader learns (facts_told, judged) where any run has them, else the atoms
// its article tells (atoms_told, code).
[[nodiscard]] std::string default_quality(std::span<const RunScores> runs);

// The page (see the header comment), runs as given: newest first is the
// caller's order.
[[nodiscard]] std::string eval_page(std::span<const RunScores> runs, const PageOptions& options);

}  // namespace rr::eval

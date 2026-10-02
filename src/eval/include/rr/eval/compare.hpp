// rr/eval/compare.hpp — two runs side by side, metric by metric: what
// `rreport eval compare` prints.
//
// Why this exists
//   A gate answers one question by a file's rules (rr/eval/gate.hpp). A
//   person deciding what to try next needs every metric: what got better,
//   what got worse, by how much, how sure, and how small a difference the
//   videos could have seen at all (ADR-0019). Each row is one metric's
//   Comparison (rr/eval/stats.hpp): the difference and its interval, its
//   standard error and 2.8 of them, the gate's margin where the gate holds
//   it, and a word for what the interval shows. Rows are grouped by the
//   scorer that computed them, code first, so a reader sees at once which
//   numbers rest on judges not yet calibrated. A metric read pairwise (flow)
//   has its row where the registry lists it, with no A and no B: its values
//   are B against A already.
//
// Thread-safety: pure functions.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/eval/gate.hpp>
#include <rr/eval/metrics.hpp>
#include <rr/eval/run.hpp>
#include <rr/eval/stats.hpp>

namespace rr::eval {

struct MetricRow {
    Metric metric{};
    std::optional<Comparison> comparison{};  // none when the runs cannot be compared on it: `why` says why
    std::optional<double> margin{};          // the gate's, where it holds the metric
    std::string why{};

    friend bool operator==(const MetricRow&, const MetricRow&) = default;
};

// What a row's interval shows.
enum class Seen : std::uint8_t {
    better,  // wholly on the better side of zero
    worse,
    higher,  // the same, for a metric better neither way
    lower,
    none,    // it holds zero: no difference these videos see
    unknown  // not compared, or fewer than two videos
};

[[nodiscard]] std::string_view to_string(Seen seen) noexcept;
[[nodiscard]] Seen seen_in(const MetricRow& row) noexcept;

// A row for every metric either run has, and for each of `read_pairwise`
// (compare_pairwise()), in metrics() order, with the margin `gated` gives
// it, if any.
[[nodiscard]] std::vector<MetricRow> compare_runs(std::span<const ScoredEntry> a,
                                                  std::span<const ScoredEntry> b,
                                                  std::span<const Gated> gated = {},
                                                  std::span<const Comparison> read_pairwise = {});

// As `rreport eval compare` prints the rows, A and B named by their labels.
[[nodiscard]] std::string to_text(std::string_view a, std::string_view b, const std::vector<MetricRow>& rows);

// One metric, video by video: each video's A, B and B − A, and a video one
// run scored and not the other with its one value. Errc::not_found for a
// metric this rreport does not score.
[[nodiscard]] Result<std::string> by_video_text(std::string_view a, std::string_view b,
                                                std::span<const ScoredEntry> a_entries,
                                                std::span<const ScoredEntry> b_entries,
                                                std::string_view metric);

// A metric read pairwise, video by video: each video's value, B against A.
[[nodiscard]] std::string by_video_text(std::string_view a, std::string_view b,
                                        const Comparison& read_pairwise);

}  // namespace rr::eval

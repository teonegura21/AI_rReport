// rr/eval/stats.hpp — two runs compared with the honesty a decision needs
// (ADR-0019): video by video, paired, with an interval from resampling the
// videos, and how small a difference the videos could see at all.
//
// Why this exists
//   A run is a sample twice over: of videos, from every video rReport will
//   read; and of the models' draws, on each video. A difference between two
//   runs' means may be the change, or chance. So a comparison says how big
//   the difference is, how sure it is, and how big a difference so many
//   videos could see:
//     1. The video is the unit. A video's replicates share the video, so
//        they are averaged first: resampling entries would count one video
//        three times as three, and claim a certainty the set does not give.
//     2. Paired: each video's difference, B − A, on the videos both runs
//        scored. Videos differ from each other far more than two runs of one
//        video do; pairing takes those differences out. Replicate r of both
//        runs draws with the same salt (ADR-0020), and stages a change leaves
//        alone are the same stage runs (ADR-0004): the difference measures
//        the change, not the dice.
//     3. The interval: the mean difference over 10,000 resamples of the
//        videos, drawn with replacement (the paired bootstrap; Koehn, 2004),
//        its 2.5th and 97.5th percentiles. The generator is seeded from the
//        metric's name, so the same scores give the same interval on every
//        machine, every time.
//     4. What could be seen: 2.8 standard errors of the mean difference
//        (z₀.₉₇₅ + z₀.₈ ≈ 1.96 + 0.84), the smallest difference these videos
//        detect four times in five. A comparison that finds none says how
//        small "none" is.
//   A metric read pairwise, between the two runs' articles (flow,
//   rr/eval/flow.hpp), has a value for each video that is a difference
//   already, B against A: it is compared as its differences alone
//   (compare_pairwise()), with no A and no B of its own.
//   And the run-to-run noise a gate's margins must stand above comes from an
//   A/A comparison: the baseline run again under other salts, nothing shared
//   (unchanged_pass_rate(); rr/eval/gate.hpp says how a gate uses it). Two
//   runs scored by different versions of a scorer measure different things:
//   comparing them is refused, and says which to score again.
//
// Thread-safety: pure functions.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/eval/run.hpp>

namespace rr::eval {

inline constexpr std::size_t kResamples = 10'000;

// z₀.₉₇₅: a 95% interval's half-width, in standard errors.
inline constexpr double kZ95 = 1.959964;

// z₀.₉₇₅ + z₀.₈ ≈ 2.8, in standard errors: the smallest difference a test at
// 5% sees four times in five; and the margin one gated metric alone needs
// for an unchanged pipeline to pass it four times in five (ADR-0019).
inline constexpr double kDetectable = kZ95 + 0.841621;

// A metric's values in a run.
struct VideoValues {
    std::map<std::string, double, std::less<>> means{};  // each video's, over its replicates that have it
    std::set<std::string, std::less<>> scorers{};        // what computed them: one, unless re-scored in part

    friend bool operator==(const VideoValues&, const VideoValues&) = default;
};

[[nodiscard]] VideoValues per_video(std::span<const ScoredEntry> entries, std::string_view metric);

struct Interval {
    double low = 0;
    double high = 0;

    friend bool operator==(const Interval&, const Interval&) = default;
};

// The 95% interval of the mean of `values`, over `resamples` resamples drawn
// with replacement by a generator seeded with `seed`. None for no values, or
// no resample.
[[nodiscard]] std::optional<Interval> bootstrap_mean(std::span<const double> values, std::uint64_t seed,
                                                     std::size_t resamples = kResamples);

// A seed of its own for each metric: its name, hashed (FNV-1a, 64 bits).
[[nodiscard]] std::uint64_t seed_of(std::string_view metric) noexcept;

// Two runs, A and B, on one metric.
struct Comparison {
    std::string metric{};
    std::string scorer{};                // what computed it in both runs; empty when neither has it
    std::size_t videos = 0;              // scored by both
    std::optional<double> a{};           // the mean over those videos; none for a metric read pairwise, which
    std::optional<double> b{};           // has no value of its own in either run (and none for no video)
    double difference = 0;               // b − a, or, read pairwise, the mean of the videos' own
    std::optional<Interval> interval{};  // of the difference; none for fewer than two videos
    std::optional<double> standard_error{};  // of the mean difference, sd / √n; none for fewer than two
    std::map<std::string, double, std::less<>> differences{};  // each video's, b − a
    std::vector<std::string> unpaired{};  // videos one run scored and not the other, left out

    // The smallest difference these videos detect four times in five: 2.8
    // standard errors.
    [[nodiscard]] std::optional<double> detectable() const;

    friend bool operator==(const Comparison&, const Comparison&) = default;
};

// `metric` in `b` against `a`. Errc::conflict when the two were computed by
// different scorers (code@1 against code@2), or one run by several: they
// measure different things, and the message says which run to score again.
[[nodiscard]] Result<Comparison> compare(std::span<const ScoredEntry> a, std::span<const ScoredEntry> b,
                                         std::string_view metric);

// `metric`, read pairwise by `scorer`: `differences` holds each video's
// value, B against A already (flow: B's preference, from −1 to +1). Its
// interval and standard error as compare() gives any difference's.
[[nodiscard]] Comparison compare_pairwise(std::string_view metric, std::string scorer,
                                          std::map<std::string, double, std::less<>> differences);

// A gated metric in an A/A comparison: each video's difference between two
// runs of one pipeline, oriented so that higher is better (a metric better
// lower enters negated), and the gate's margin for it.
struct NullMetric {
    std::string metric{};
    std::map<std::string, double, std::less<>> differences{};
    double margin = 0;

    friend bool operator==(const NullMetric&, const NullMetric&) = default;
};

// How often a pipeline no different from the baseline passes every metric of
// `metrics` at once (ADR-0019), on the videos they all have: each metric's
// differences centred on zero (an unchanged pipeline differs by chance
// alone), the videos resampled `resamples` times, one draw for all metrics,
// so that metrics that move together are counted together. A draw passes a
// metric when its mean, less 1.96 of the metric's standard errors, is its
// margin's negative or more: the gate's rule, on the interval's normal
// approximation. None for fewer than two videos all metrics have.
[[nodiscard]] std::optional<double> unchanged_pass_rate(std::span<const NullMetric> metrics,
                                                        std::uint64_t seed,
                                                        std::size_t resamples = kResamples);

// How often one metric alone, with standard error `standard_error`, passes
// at `margin` when nothing changed: Φ(margin / standard_error − 1.96).
[[nodiscard]] double alone_pass_rate(double margin, double standard_error) noexcept;

}  // namespace rr::eval

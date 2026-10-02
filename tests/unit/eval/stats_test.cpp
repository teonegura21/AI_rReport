#include <rr/eval/stats.hpp>

#include <cmath>
#include <format>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>

namespace rr::eval {
namespace {

using ::testing::DoubleNear;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Optional;
using ::testing::Pair;
using testkit::unwrap;

// An entry of `video`'s replicate `replicate`, scored by `scorer`.
ScoredEntry entry(std::string video, int replicate, std::vector<Score> scores,
                  std::string scorer = "code@1") {
    return ScoredEntry{.entry = RunEntry{.video = std::move(video), .replicate = replicate, .job = "J"},
                       .scorings = {Scoring{.scorer = std::move(scorer), .scores = std::move(scores)}}};
}

ScoredEntry told(std::string video, int replicate, double value, std::string scorer = "code@1") {
    return entry(std::move(video), replicate, {Score{.metric = "atoms_told", .value = value}},
                 std::move(scorer));
}

TEST(PerVideo, IsTheMeanOfEachVideosReplicatesThatHaveIt) {
    const std::vector<ScoredEntry> run{told("lecture", 0, 0.9), told("lecture", 1, 1.0),
                                       entry("lecture", 2, {Score{.metric = "invalid_markers", .value = 0}}),
                                       told("podcast", 0, 0.5)};
    const VideoValues values = per_video(run, "atoms_told");
    EXPECT_THAT(values.means, ElementsAre(Pair("lecture", DoubleNear(0.95, 1e-12)), Pair("podcast", 0.5)))
        << "replicate 2 has no atoms_told: left out, not counted as nothing";
    EXPECT_THAT(values.scorers, ElementsAre("code@1"));
    EXPECT_THAT(per_video(run, "facts_told").means, IsEmpty());
}

TEST(SeedOf, IsTheNamesFnv1a) {
    // FNV-1a's published 64-bit test vectors.
    EXPECT_EQ(seed_of(""), 0xcbf29ce484222325ULL);
    EXPECT_EQ(seed_of("a"), 0xaf63dc4c8601ec8cULL);
    EXPECT_EQ(seed_of("foobar"), 0x85944171f73967e8ULL);
}

TEST(BootstrapMean, OfEqualValuesHasNoWidth) {
    const std::vector<double> values{0.02, 0.02, 0.02};
    EXPECT_EQ(bootstrap_mean(values, 7), (Interval{.low = 0.02, .high = 0.02}));
}

TEST(BootstrapMean, OfTwoValuesSpansThem) {
    // A resample's mean is 0, ½ or 1, a quarter, a half and a quarter of the
    // time: the 2.5th and 97.5th percentiles are the two values.
    const std::vector<double> values{0.0, 1.0};
    EXPECT_EQ(bootstrap_mean(values, 7), (Interval{.low = 0.0, .high = 1.0}));
}

TEST(BootstrapMean, IsTheSameOnEveryMachine) {
    // Pinned by an independent reference: mt19937_64 written in Python from
    // the standard's definition (which it checks against the standard's own
    // 10000th number), the same resampling, the same nearest ranks.
    const std::vector<double> values{0.10, -0.05, 0.20, 0.00, 0.15};
    const std::optional<Interval> interval = bootstrap_mean(values, seed_of("atoms_told"));
    ASSERT_TRUE(interval.has_value());
    EXPECT_DOUBLE_EQ(interval->low, 0.0);
    EXPECT_DOUBLE_EQ(interval->high, 0.16);
    EXPECT_EQ(bootstrap_mean(values, seed_of("atoms_told")), interval)
        << "the same scores, the same interval";
}

TEST(BootstrapMean, OfNothingIsNone) {
    EXPECT_EQ(bootstrap_mean({}, 7), std::nullopt);
    const std::vector<double> values{0.1, 0.2};
    EXPECT_EQ(bootstrap_mean(values, 7, 0), std::nullopt) << "no resample";
}

TEST(Compare, PairsTheVideosBothRunsScoredAndLeavesOutTheRest) {
    const std::vector<ScoredEntry> a{told("lecture", 0, 0.8), told("podcast", 0, 0.6),
                                     told("tutorial", 0, 0.9)};
    const std::vector<ScoredEntry> b{told("lecture", 0, 0.9), told("podcast", 0, 0.6), told("news", 0, 1.0)};
    const Comparison compared = unwrap(compare(a, b, "atoms_told"));
    EXPECT_EQ(compared.metric, "atoms_told");
    EXPECT_EQ(compared.scorer, "code@1");
    EXPECT_EQ(compared.videos, 2U);
    EXPECT_THAT(compared.unpaired, ElementsAre("news", "tutorial"));
    EXPECT_THAT(compared.a, Optional(DoubleNear(0.7, 1e-12))) << "A's mean over the videos B scored too";
    EXPECT_THAT(compared.b, Optional(DoubleNear(0.75, 1e-12)));
    EXPECT_THAT(compared.difference, DoubleNear(0.05, 1e-12));
    EXPECT_THAT(compared.differences, ElementsAre(Pair("lecture", DoubleNear(0.1, 1e-12)),
                                                  Pair("podcast", DoubleNear(0.0, 1e-12))));
    // sd(0.1, 0) = 0.0707…, over √2: 0.05; 2.8 of them: 0.140.
    ASSERT_TRUE(compared.standard_error.has_value());
    EXPECT_THAT(*compared.standard_error, DoubleNear(0.05, 1e-12));
    EXPECT_THAT(compared.detectable().value_or(0), DoubleNear(2.801585 * 0.05, 1e-9))
        << "z of 0.975 and of 0.8, in standard errors";
    ASSERT_TRUE(compared.interval.has_value());
    EXPECT_LE(compared.interval->low, compared.difference);
    EXPECT_GE(compared.interval->high, compared.difference);
    EXPECT_EQ(compared.interval,
              bootstrap_mean(std::vector<double>{0.9 - 0.8, 0.6 - 0.6}, seed_of("atoms_told")))
        << "the videos' differences resampled, seeded by the metric's name";
}

TEST(Compare, ResamplesTheVideosUnderTheMetricsOwnSeed) {
    // Twelve videos, so that the interval shows the draw: pinned by the
    // Python reference, which gives another under any other seed (seed 0:
    // 0.0135 to 0.0425).
    const std::vector<double> a{0.812, 0.705, 0.633, 0.904, 0.551, 0.778,
                                0.689, 0.842, 0.716, 0.597, 0.871, 0.664};
    // 0.693 is a score, not ln 2.
    // NOLINTNEXTLINE(modernize-use-std-numbers)
    const std::vector<double> b{0.851, 0.693, 0.702, 0.917, 0.603, 0.771,
                                0.734, 0.866, 0.709, 0.655, 0.902, 0.698};
    std::vector<ScoredEntry> in_a;
    std::vector<ScoredEntry> in_b;
    for (std::size_t video = 0; video < a.size(); ++video) {
        in_a.push_back(told(std::format("v{:02}", video + 1), 0, a[video]));
        in_b.push_back(told(std::format("v{:02}", video + 1), 0, b[video]));
    }
    const Comparison compared = unwrap(compare(in_a, in_b, "atoms_told"));
    ASSERT_TRUE(compared.interval.has_value());
    EXPECT_DOUBLE_EQ(compared.interval->low, 0.01391666666666666);
    EXPECT_DOUBLE_EQ(compared.interval->high, 0.04275);
}

TEST(Compare, AveragesAVideosReplicatesBeforeItResamplesTheVideos) {
    const std::vector<ScoredEntry> a{told("lecture", 0, 0.6), told("lecture", 1, 0.8),
                                     told("lecture", 2, 1.0), told("podcast", 0, 0.5)};
    const std::vector<ScoredEntry> b{told("lecture", 0, 0.9), told("podcast", 0, 0.5)};
    const Comparison compared = unwrap(compare(a, b, "atoms_told"));
    EXPECT_EQ(compared.videos, 2U) << "two videos, not four entries";
    EXPECT_THAT(compared.differences, ElementsAre(Pair("lecture", DoubleNear(0.1, 1e-12)),
                                                  Pair("podcast", DoubleNear(0.0, 1e-12))));
}

TEST(Compare, OfOneVideoHasNoIntervalAndNoStandardError) {
    const Comparison compared = unwrap(
        compare(std::vector{told("lecture", 0, 0.8)}, std::vector{told("lecture", 0, 0.9)}, "atoms_told"));
    EXPECT_EQ(compared.videos, 1U);
    EXPECT_THAT(compared.difference, DoubleNear(0.1, 1e-12));
    EXPECT_EQ(compared.interval, std::nullopt);
    EXPECT_EQ(compared.standard_error, std::nullopt);
    EXPECT_EQ(compared.detectable(), std::nullopt);
}

TEST(Compare, OfAMetricNeitherRunHasIsEmpty) {
    const Comparison compared = unwrap(
        compare(std::vector{told("lecture", 0, 0.8)}, std::vector{told("lecture", 0, 0.9)}, "facts_told"));
    EXPECT_EQ(compared.scorer, "");
    EXPECT_EQ(compared.videos, 0U);
    EXPECT_EQ(compared.a, std::nullopt) << "no video, no mean";
    EXPECT_THAT(compared.unpaired, IsEmpty());
}

TEST(ComparePairwise, IsOfTheVideosOwnDifferencesWithNoAAndNoB) {
    const std::map<std::string, double, std::less<>> preferences{
        {"lecture", 0.5}, {"podcast", 0.0}, {"tutorial", 1.0}};
    const Comparison compared = compare_pairwise("flow", "judges@eval-v1", preferences);
    EXPECT_EQ(compared.metric, "flow");
    EXPECT_EQ(compared.scorer, "judges@eval-v1");
    EXPECT_EQ(compared.videos, 3U);
    EXPECT_EQ(compared.a, std::nullopt) << "no run has a flow of its own";
    EXPECT_EQ(compared.b, std::nullopt);
    EXPECT_THAT(compared.difference, DoubleNear(0.5, 1e-12));
    EXPECT_EQ(compared.differences, preferences);
    // sd(0.5, 0, 1) = 0.5, over √3.
    EXPECT_THAT(compared.standard_error, Optional(DoubleNear(0.5 / std::sqrt(3.0), 1e-12)));
    EXPECT_EQ(compared.interval, bootstrap_mean(std::vector<double>{0.5, 0.0, 1.0}, seed_of("flow")))
        << "resampled as any difference, under the metric's own seed";

    const Comparison one = compare_pairwise("flow", "judges@eval-v1", {{"lecture", -1.0}});
    EXPECT_EQ(one.difference, -1.0);
    EXPECT_EQ(one.interval, std::nullopt) << "one video: no interval";
    EXPECT_EQ(one.standard_error, std::nullopt);
    const Comparison none = compare_pairwise("flow", "judges@eval-v1", {});
    EXPECT_EQ(none.videos, 0U);
    EXPECT_EQ(none.difference, 0.0);
}

TEST(Compare, RefusesRunsScoredByScorersThatMeasureDifferently) {
    const auto across = compare(std::vector{told("lecture", 0, 0.8)},
                                std::vector{told("lecture", 0, 0.9, "code@2")}, "atoms_told");
    EXPECT_ERRC(across, Errc::conflict);
    EXPECT_THAT(across.error().message(), HasSubstr("run A was scored by code@1 and run B by code@2"));

    const auto within = compare(std::vector{told("lecture", 0, 0.8), told("podcast", 0, 0.7, "code@2")},
                                std::vector{told("lecture", 0, 0.9)}, "atoms_told");
    EXPECT_ERRC(within, Errc::conflict);
    EXPECT_THAT(within.error().message(), HasSubstr("run A's entries were scored by code@1 and code@2"));
    EXPECT_THAT(within.error().message(), HasSubstr("score the run again, whole"));
}

// Forty videos whose differences are ±d: an A/A comparison of known noise.
NullMetric alternating(std::string metric, double size, double margin) {
    NullMetric null{.metric = std::move(metric), .margin = margin};
    for (int video = 0; video < 40; ++video) {
        null.differences.emplace(std::format("v{:02}", video), video % 2 == 0 ? size : -size);
    }
    return null;
}

TEST(UnchangedPassRate, IsHowOftenAnUnchangedPipelinePassesItsMargin) {
    // ±d over 40 videos: SE = d/√39. A resample's mean is d(K/20 − 1), K the
    // draws of +d out of 40, Binomial(40, ½); it passes a margin of 2.8 SE
    // when its mean less 1.96 SE is −2.8 SE or more: K ≥ 18, 78.5% of the time.
    const double size = 0.01;
    const double standard_error = size / std::sqrt(39.0);
    const std::vector<NullMetric> one{alternating("atoms_told", size, kDetectable * standard_error)};
    EXPECT_THAT(unchanged_pass_rate(one, 7).value_or(-1), DoubleNear(0.785, 0.015));
    EXPECT_THAT(alone_pass_rate(kDetectable * standard_error, standard_error), DoubleNear(0.8, 1e-6))
        << "the normal approximation, alone";
}

TEST(UnchangedPassRate, CountsMetricsThatMoveTogetherOnce) {
    // Two metrics with the same differences pass or fail together: their
    // joint rate is either one's, not its square.
    const double size = 0.01;
    const double margin = kDetectable * size / std::sqrt(39.0);
    const std::vector<NullMetric> twins{alternating("atoms_told", size, margin),
                                        alternating("numbers_kept", size, margin)};
    EXPECT_EQ(unchanged_pass_rate(twins, 7), unchanged_pass_rate(std::vector{twins.front()}, 7));
}

TEST(UnchangedPassRate, WideAndTightMarginsPassAlwaysAndRarely) {
    const std::vector<NullMetric> wide{alternating("atoms_told", 0.01, 1.0)};
    EXPECT_EQ(unchanged_pass_rate(wide, 7), 1.0);
    const std::vector<NullMetric> none{alternating("atoms_told", 0.01, 0.0)};
    EXPECT_LT(unchanged_pass_rate(none, 7).value_or(1), 0.1) << "a margin of nothing fails on noise alone";
}

TEST(UnchangedPassRate, CentresEachMetricOnZeroAsAnUnchangedPipelineDiffersByChanceAlone) {
    // An A/A whose every video differs by the same 0.03: no noise at all, so
    // an unchanged pipeline passes any margin, its constant shift aside.
    NullMetric shifted{.metric = "atoms_told", .margin = 0.02};
    for (int video = 0; video < 10; ++video) {
        shifted.differences.emplace(std::format("v{}", video), -0.03);
    }
    EXPECT_EQ(unchanged_pass_rate(std::vector{shifted}, 7), 1.0);
}

TEST(UnchangedPassRate, OfNoMetricIsOneAndOfTooFewVideosNone) {
    EXPECT_EQ(unchanged_pass_rate({}, 7), 1.0) << "every metric of none passes";
    NullMetric one{.metric = "atoms_told", .differences = {{"lecture", 0.01}}, .margin = 0.01};
    EXPECT_EQ(unchanged_pass_rate(std::vector{one}, 7), std::nullopt);
    NullMetric other = one;
    other.differences = {{"podcast", 0.02}};
    one.differences.emplace("tutorial", 0.0);
    other.differences.emplace("tutorial", 0.0);
    EXPECT_EQ(unchanged_pass_rate(std::vector{one, other}, 7), std::nullopt)
        << "only one video both metrics have";
}

TEST(AlonePassRate, IsPhiOfTheMarginInStandardErrorsLessTheIntervalsHalfWidth) {
    EXPECT_THAT(alone_pass_rate(kZ95 * 0.01, 0.01), DoubleNear(0.5, 1e-9));
    EXPECT_THAT(alone_pass_rate(0.0, 0.01), DoubleNear(0.025, 1e-4));
    EXPECT_EQ(alone_pass_rate(0.0, 0.0), 1.0) << "no noise: an unchanged pipeline differs by nothing";
}

}  // namespace
}  // namespace rr::eval

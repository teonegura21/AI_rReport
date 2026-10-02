// The eval's runs on one page: each run's means and their intervals, the
// chart's axes on round steps, and the page itself.
#include <rr/eval/history.hpp>

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace rr::eval {
namespace {

using ::testing::DoubleNear;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::Not;

Score at(std::string metric, double value) {
    return Score{.metric = std::move(metric), .value = value};
}

ScoredEntry scored(std::string video, int replicate, std::vector<Score> code,
                   std::vector<Score> judged = {}) {
    ScoredEntry entry{.entry = RunEntry{.video = std::move(video), .replicate = replicate, .job = "J"}};
    entry.scorings.push_back(Scoring{.scorer = "code@1", .scores = std::move(code)});
    if (!judged.empty()) {
        entry.scorings.push_back(Scoring{.scorer = "judges@eval-v1", .scores = std::move(judged)});
    }
    return entry;
}

TEST(RunMean, IsTheMeanOverVideosOfEachVideosReplicatesWithItsInterval) {
    const std::vector<ScoredEntry> scores{scored("lecture", 0, {at("atoms_told", 0.8)}),
                                          scored("lecture", 1, {at("atoms_told", 1.0)}),
                                          scored("podcast", 0, {at("atoms_told", 0.6)})};
    const std::optional<RunMean> mean = run_mean(scores, "atoms_told");
    ASSERT_TRUE(mean.has_value());
    EXPECT_THAT(mean->mean, DoubleNear(0.75, 1e-12))
        << "(0.9 + 0.6) / 2: the video is the unit, not the entry";
    EXPECT_EQ(mean->videos, 2U);
    EXPECT_EQ(mean->interval,
              bootstrap_mean(std::vector<double>{(0.8 + 1.0) / 2, 0.6}, seed_of("atoms_told")))
        << "the videos resampled under the metric's own seed";
    EXPECT_EQ(run_mean(scores, "facts_told"), std::nullopt);
    EXPECT_EQ(run_mean(std::vector{scored("lecture", 0, {at("atoms_told", 0.8)})}, "atoms_told")->interval,
              std::nullopt)
        << "one video: no interval";
}

TEST(AxisOver, HoldsEveryValueOnRoundTicks) {
    const Axis shares = axis_over(std::vector<double>{0.62, 0.91, 0.75});
    EXPECT_THAT(shares.low, DoubleNear(0.6, 1e-12));
    EXPECT_THAT(shares.high, DoubleNear(1.0, 1e-12));
    EXPECT_THAT(shares.step, DoubleNear(0.1, 1e-12));
    ASSERT_EQ(shares.ticks.size(), 5U);
    EXPECT_THAT(shares.ticks.back(), DoubleNear(1.0, 1e-12));

    const Axis minutes = axis_over(std::vector<double>{12.0, 87.0});
    EXPECT_EQ(minutes.step, 20.0) << "75 over five: 15, up to 20";
    EXPECT_EQ(minutes.low, 0.0);
    EXPECT_EQ(minutes.high, 100.0);
    EXPECT_THAT(minutes.ticks, ElementsAre(0.0, 20.0, 40.0, 60.0, 80.0, 100.0));

    EXPECT_EQ(axis_over(std::vector<double>{3.0, 4.0}).step, 0.2) << "a fifth of 1: 0.2";
    // A value a division's rounding puts a hair off a tick is on it, with no
    // step of room: 0.6 / 0.1 is 5.999…, and 0.07 / 0.01 is 7.000…1.
    EXPECT_THAT(axis_over(std::vector<double>{0.6, 0.9}).low, DoubleNear(0.6, 1e-12));
    EXPECT_THAT(axis_over(std::vector<double>{0.02, 0.07}).high, DoubleNear(0.07, 1e-12));
    EXPECT_EQ(axis_over(std::vector<double>{0.0, 25.0}).step, 5.0) << "a fifth of 25: 5";

    const Axis one = axis_over(std::vector<double>{40.0});
    EXPECT_LT(one.low, 40.0) << "one value: room around it";
    EXPECT_GT(one.high, 40.0);
    const Axis zero = axis_over(std::vector<double>{0.0, 0.0});
    EXPECT_LT(zero.low, 0.0);
    EXPECT_GT(zero.high, 0.0);
}

RunScores run_of(std::string label, std::vector<ScoredEntry> scores) {
    return RunScores{
        .run =
            EvalRun{
                .id = label + "-id",
                .spec = RunSpec{.label = std::move(label), .eval_set = "m2", .tier = "max", .replicates = 2},
                .created_at = std::chrono::sys_days{std::chrono::year{2026} / 10 / 2}},
        .scores = std::move(scores)};
}

std::vector<ScoredEntry> costed(double told, double minutes) {
    return {scored("lecture", 0, {at("atoms_told", told), at("minutes_per_video_hour", minutes)}),
            scored("podcast", 0, {at("atoms_told", told), at("minutes_per_video_hour", minutes)})};
}

TEST(DefaultQuality, IsTheFactsTheJudgesFoundToldElseTheAtomsTheCodeFound) {
    std::vector<RunScores> runs{run_of("code", costed(0.9, 40))};
    EXPECT_EQ(default_quality(runs), "atoms_told");
    runs.push_back(
        run_of("judged", {scored("lecture", 0, {at("atoms_told", 0.9)}, {at("facts_told", 0.8)})}));
    EXPECT_EQ(default_quality(runs), "facts_told");
}

TEST(EvalPage, PlotsEachRunAtItsMeansAndListsWhatItCannot) {
    const std::vector<RunScores> runs{
        run_of("cheap", costed(0.6, 20)), run_of("max", costed(0.9, 80)),
        run_of("<new>", {scored("lecture", 0, {at("atoms_told", 0.7)}, {at("facts_told", 0.5)})})};
    const std::string page =
        eval_page(runs, PageOptions{.quality = "atoms_told", .made = "2026-10-02 13:00 UTC"});
    EXPECT_THAT(page, HasSubstr("<h2>atoms_told against minutes_per_video_hour</h2>"));
    // The axes: 20 to 80 minutes on steps of 20, 0.6 to 0.9 on steps of 0.1,
    // each a value on its ends. The plot runs from x 72 to 696 and from y 384
    // up to 16.
    EXPECT_THAT(
        page,
        HasSubstr(
            R"(<circle cx="72.0" cy="384.0" r="4"><title>cheap: atoms_told 0.600 (0.600 to 0.600), minutes_per_video_hour 20.000 (20.000 to 20.000)</title></circle>)"));
    EXPECT_THAT(page, HasSubstr(R"(<circle cx="696.0" cy="16.0" r="4">)")) << "max, at the far corner";
    EXPECT_THAT(page, HasSubstr("Not plotted, missing either: &lt;new&gt;")) << "escaped, as every label";
    EXPECT_THAT(page, Not(HasSubstr("<new>")));
    EXPECT_THAT(page, HasSubstr("<td>cheap</td><td>2026-10-02 00:00</td><td>m2 dev</td><td>max</td>"
                                "<td>0 to 1</td><td class=\"n\">2</td><td class=\"n\">2</td>"
                                "<td class=\"n\">0</td><td class=\"n\">-</td><td class=\"n\">0.600</td>"
                                "<td class=\"n\">-</td><td class=\"n\">-</td><td class=\"n\">20.000</td>"
                                "</tr>"))
        << "two entries, both scored by code and neither judged, then the headline means";
    EXPECT_THAT(page, HasSubstr("<td>0 to 1</td><td class=\"n\">1</td><td class=\"n\">1</td>"
                                "<td class=\"n\">1</td><td class=\"n\">-</td><td class=\"n\">0.700</td>"
                                "<td class=\"n\">0.500</td>"))
        << "the new run's one entry, scored and judged";
    EXPECT_THAT(page, HasSubstr("<details><summary>max: 2 videos</summary>"));
    EXPECT_THAT(page,
                HasSubstr("<tr><td>lecture</td><td class=\"n\">0.900</td><td class=\"n\">80.000</td></tr>"))
        << "a row a video, a column a metric the run has";
    EXPECT_THAT(page, HasSubstr("as the library had them at 2026-10-02 13:00 UTC"));

    const std::string empty = eval_page({}, PageOptions{.quality = "atoms_told"});
    EXPECT_THAT(empty, HasSubstr("No run has both yet"));
}

}  // namespace
}  // namespace rr::eval

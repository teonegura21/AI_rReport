#include <rr/eval/compare.hpp>

#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>

namespace rr::eval {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::Not;
using testkit::unwrap;

Score at(std::string metric, double value) {
    return Score{.metric = std::move(metric), .value = value};
}

ScoredEntry scored(std::string video, int replicate, std::vector<Scoring> scorings) {
    return ScoredEntry{.entry = RunEntry{.video = std::move(video), .replicate = replicate, .job = "J"},
                       .scorings = std::move(scorings)};
}

// Two videos, each scored by the code and by the judges.
std::vector<ScoredEntry> run(double told, double facts) {
    const auto both = [&](std::string video) {
        return scored(
            std::move(video), 0,
            {Scoring{.scorer = "code@1", .scores = {at("atoms_told", told), at("length_ratio", 0.4)}},
             Scoring{.scorer = "judges@eval-v9", .scores = {at("facts_told", facts)}}});
    };
    return {both("lecture"), both("podcast")};
}

std::vector<std::string> names(const std::vector<MetricRow>& rows) {
    std::vector<std::string> named;
    named.reserve(rows.size());
    for (const MetricRow& row : rows) {
        named.emplace_back(row.metric.name);
    }
    return named;
}

TEST(CompareRuns, MakesARowForEachMetricEitherRunHasInTheRegistrysOrder) {
    const std::vector<Gated> gated{Gated{.metric = "atoms_told", .margin = 0.01}};
    const std::vector<MetricRow> rows = compare_runs(run(0.9, 0.8), run(0.95, 0.8), gated);
    EXPECT_THAT(names(rows), ElementsAre("atoms_told", "facts_told", "length_ratio"))
        << "metrics() order, not each run's";
    EXPECT_EQ(rows[0].margin, 0.01) << "the gate's margin, where it holds the metric";
    EXPECT_EQ(rows[1].margin, std::nullopt);
    ASSERT_TRUE(rows[0].comparison.has_value());
    EXPECT_EQ(rows[0].comparison->scorer, "code@1");
    EXPECT_EQ(rows[1].comparison->scorer, "judges@eval-v9");
}

TEST(CompareRuns, ARowTheRunsCannotBeComparedOnSaysWhy) {
    std::vector<ScoredEntry> rescored = run(0.9, 0.8);
    for (ScoredEntry& entry : rescored) {
        entry.scorings[0].scorer = "code@2";
    }
    const std::vector<MetricRow> rows = compare_runs(run(0.9, 0.8), rescored);
    EXPECT_EQ(rows[0].comparison, std::nullopt);
    EXPECT_THAT(rows[0].why, HasSubstr("atoms_told: run A was scored by code@1 and run B by code@2"));
}

MetricRow row_of(std::string_view metric, Interval interval) {
    return MetricRow{
        .metric = *find_metric(metric),
        .comparison = Comparison{.metric = std::string{metric}, .videos = 2, .interval = interval}};
}

TEST(SeenIn, IsWhatTheIntervalShowsTheWayTheMetricIsBetter) {
    EXPECT_EQ(seen_in(row_of("atoms_told", {.low = 0.01, .high = 0.03})), Seen::better);
    EXPECT_EQ(seen_in(row_of("atoms_told", {.low = -0.03, .high = -0.01})), Seen::worse);
    EXPECT_EQ(seen_in(row_of("atoms_told", {.low = -0.01, .high = 0.01})), Seen::none);
    EXPECT_EQ(seen_in(row_of("atoms_told", {.low = 0.0, .high = 0.0})), Seen::none) << "no difference at all";
    EXPECT_EQ(seen_in(row_of("contradicts", {.low = 0.01, .high = 0.03})), Seen::worse) << "better lower";
    EXPECT_EQ(seen_in(row_of("contradicts", {.low = -0.03, .high = -0.01})), Seen::better);
    EXPECT_EQ(seen_in(row_of("length_ratio", {.low = 0.01, .high = 0.03})), Seen::higher)
        << "better neither way";
    EXPECT_EQ(seen_in(row_of("length_ratio", {.low = -0.03, .high = -0.01})), Seen::lower);
    EXPECT_EQ(seen_in(MetricRow{.metric = *find_metric("atoms_told")}), Seen::unknown) << "not compared";
    EXPECT_EQ(to_string(Seen::none), "no difference");
    EXPECT_EQ(to_string(Seen::unknown), "too few videos");
}

TEST(CompareText, GroupsTheRowsByScorerCodeFirstAndSaysWhatWasLeftOut) {
    std::vector<ScoredEntry> b = run(0.95, 0.8);
    b.push_back(scored("tutorial", 0, {Scoring{.scorer = "code@1", .scores = {at("atoms_told", 1.0)}}}));
    std::vector<MetricRow> rows =
        compare_runs(run(0.9, 0.8), b, std::vector{Gated{.metric = "atoms_told", .margin = 0.01}});
    rows.push_back(
        MetricRow{.metric = *find_metric("numbers_kept"), .why = "numbers_kept: not compared, so"});
    const std::string text = to_text("base (m2 dev)", "max (m2 dev)", rows);
    EXPECT_THAT(text, HasSubstr("A: base (m2 dev)\nB: max (m2 dev)\n"));
    const std::size_t code = text.find("\ncode@1\n");
    const std::size_t judged = text.find("\njudges@eval-v9\n");
    ASSERT_NE(code, std::string::npos) << text;
    ASSERT_NE(judged, std::string::npos) << text;
    EXPECT_LT(code, judged) << "the code's rows first";
    EXPECT_LT(text.find("length_ratio"), judged)
        << "a code metric with the code's, wherever metrics() has it";
    EXPECT_THAT(text,
                HasSubstr("  atoms_told                    2     0.900     0.950    +0.050   +0.050 to +0.050"
                          "       0.000   0.000   0.010   better\n"));
    EXPECT_THAT(text, HasSubstr("\nnot compared:\n  numbers_kept: not compared, so\n"));
    EXPECT_THAT(text, HasSubstr("\nleft out, scored by one run and not the other: tutorial\n"));
    EXPECT_THAT(to_text("A", "B", {}), HasSubstr("Neither run has a score yet"));
    EXPECT_THAT(to_text("A", "B", rows), Not(HasSubstr("noise")));
}

TEST(ByVideoText, ListsEachVideoOfEitherRun) {
    std::vector<ScoredEntry> b = run(0.95, 0.8);
    b.pop_back();
    const std::string text = unwrap(by_video_text("A", "B", run(0.9, 0.8), b, "atoms_told"));
    EXPECT_THAT(text, HasSubstr("atoms_told, video by video"));
    EXPECT_THAT(text, HasSubstr("  lecture                          0.900     0.950    +0.050\n"));
    EXPECT_THAT(text, HasSubstr("  podcast                          0.900         -         -\n"));
    EXPECT_ERRC(by_video_text("A", "B", {}, {}, "atoms_tolt"), Errc::not_found);
    EXPECT_THAT(unwrap(by_video_text("A", "B", {}, {}, "atoms_told")), HasSubstr("neither run has it"));
}

}  // namespace
}  // namespace rr::eval

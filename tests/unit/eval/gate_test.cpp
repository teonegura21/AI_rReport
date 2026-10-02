#include <rr/eval/gate.hpp>

#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>

namespace rr::eval {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using testkit::unwrap;

const Panel kPanel{.name = "eval-v9"};  // no person has calibrated it
const Panel kCalibrated{.name = "eval-v9", .calibrated = true};

Score at(std::string metric, double value) {
    return Score{.metric = std::move(metric), .value = value};
}

Scoring code(std::vector<Score> scores) {
    return Scoring{.scorer = "code@1", .scores = std::move(scores)};
}

Scoring judges(std::vector<Score> scores, std::string panel = "eval-v9") {
    return Scoring{.scorer = std::format("judges@{}", panel), .scores = std::move(scores)};
}

ScoredEntry scored(std::string video, int replicate, std::vector<Scoring> scorings) {
    return ScoredEntry{.entry = RunEntry{.video = std::move(video), .replicate = replicate, .job = "J"},
                       .scorings = std::move(scorings)};
}

// A run of two videos, one replicate each, with `metric` as given.
std::vector<ScoredEntry> two_videos(std::string_view metric, double lecture, double podcast,
                                    std::string_view scorer = "code@1") {
    const auto scoring = [&](double value) {
        return Scoring{.scorer = std::string{scorer},
                       .scores = {Score{.metric = std::string{metric}, .value = value}}};
    };
    return {scored("lecture", 0, {scoring(lecture)}), scored("podcast", 0, {scoring(podcast)})};
}

const Gate kAtomsTold{.name = "g", .gated = {Gated{.metric = "atoms_told", .margin = 0.01}}};

TEST(Gate, TheShippedDefaultHoldsTheInvariantsAndGatesQualityNeverCost) {
    const Gate gate = unwrap(read_gate(std::filesystem::path{RR_SOURCE_DIR} / "eval", "default"));
    EXPECT_EQ(gate.name, "default");
    EXPECT_THAT(gate.invariants, ElementsAre(Invariant{.metric = "sentences_accounted", .every_entry = 1},
                                             Invariant{.metric = "invalid_markers", .every_entry = 0},
                                             Invariant{.metric = "quotes_kept", .every_entry = 1}));
    ASSERT_FALSE(gate.gated.empty());
    for (const Gated& gated : gate.gated) {
        const Metric* metric = find_metric(gated.metric);
        ASSERT_NE(metric, nullptr) << gated.metric;
        EXPECT_FALSE(metric->cost) << gated.metric;
        EXPECT_GT(gated.margin, 0) << gated.metric;
    }
}

TEST(Gate, EveryMistakeIsRefusedNamingItAndWhereItIs) {
    const auto refused = [](std::string_view text, std::string_view said) {
        const auto parsed = parse_gate("g", text);
        ASSERT_FALSE(parsed.has_value()) << text;
        EXPECT_EQ(parsed.error().code(), Errc::invalid_argument) << text;
        EXPECT_THAT(parsed.error().message(), HasSubstr(said)) << text;
    };
    constexpr std::string_view kGated = "[[gated]]\nmetric = \"atoms_told\"\nmargin = 0.01\n";
    refused(std::format("name = \"other\"\n{}", kGated), R"('name' must be the file's, "g", not "other")");
    refused(std::format("name = \"g\"\nmargins = 1\n{}", kGated), "unknown key 'margins'");
    refused("name = \"g\"\n[[gated]]\nmetric = \"atoms_tolt\"\nmargin = 0.01\n",
            "eval/gates/g.toml, gated 1: 'atoms_tolt' is not a metric this rreport scores");
    refused("name = \"g\"\n[[gated]]\nmetric = \"length_ratio\"\nmargin = 0.01\n",
            "better neither higher nor lower: it is shown, never gated");
    refused("name = \"g\"\n[[invariant]]\nmetric = \"minutes\"\nevery_entry = 60\n",
            "'minutes' is a cost: costs are shown, never gated");
    refused("name = \"g\"\n[[gated]]\nmetric = \"flow\"\nmargin = 0.1\n",
            "'flow' is read pairwise, by `rreport eval compare --flow`, and no run scores it");
    refused(std::format("name = \"g\"\n{}{}", kGated, kGated), "gated 2: 'atoms_told' a second time");
    refused(
        "name = \"g\"\n[[invariant]]\nmetric = \"invalid_markers\"\nevery_entry = 0\n[[invariant]]\n"
        "metric = \"invalid_markers\"\nevery_entry = 0\n",
        "invariant 2: 'invalid_markers' a second time: one invariant a metric");
    refused("name = \"g\"\n[[gated]]\nmetric = \"atoms_told\"\nmargin = -0.01\n",
            "'margin' must be 0 or more");
    refused("name = \"g\"\n[[gated]]\nmetric = \"atoms_told\"\nmargin = \"0.01\"\n",
            "'margin' must be a number");
    refused("name = \"g\"\n[[invariant]]\nmetric = \"invalid_markers\"\n", "'every_entry' must be a number");
    refused("name = \"g\"\n[[gated]]\nmetric = \"atoms_told\"\nmargin = 0.01\nwhy = \"x\"\n",
            "gated 1: unknown key 'why'");
    refused("name = \"g\"\n", "no [[invariant]] and no [[gated]]: a gate checks something");

    const Gate whole =
        unwrap(parse_gate("g",
                          "name = \"g\"\n[[invariant]]\nmetric = \"invalid_markers\"\n"
                          "every_entry = 0\n[[gated]]\nmetric = \"atoms_told\"\nmargin = 0\n"));
    EXPECT_EQ(whole, (Gate{.name = "g",
                           .invariants = {Invariant{.metric = "invalid_markers", .every_entry = 0}},
                           .gated = {Gated{.metric = "atoms_told", .margin = 0}}}))
        << "a whole number where a number is asked for";
}

TEST(Invariants, AreKeptOnlyWhenEveryEntryIsAtLeastAsGood) {
    const Gate gate{.name = "g",
                    .invariants = {Invariant{.metric = "sentences_accounted", .every_entry = 1},
                                   Invariant{.metric = "invalid_markers", .every_entry = 0}}};
    const std::vector<ScoredEntry> candidate{
        scored("lecture", 0, {code({at("sentences_accounted", 1.0), at("invalid_markers", 0)})}),
        scored("podcast", 1, {code({at("sentences_accounted", 0.99), at("invalid_markers", 0)})})};
    const GateReading reading = read_gate_on(gate, {}, candidate, kPanel);
    ASSERT_EQ(reading.invariants.size(), 2U);
    EXPECT_EQ(reading.invariants[0].outcome, Outcome::failed) << "at least 1, and an entry is 0.99";
    EXPECT_THAT(reading.invariants[0].broken, ElementsAre("podcast, replicate 1: 0.990"));
    EXPECT_EQ(reading.invariants[1].outcome, Outcome::passed) << "at most 0, and every entry is";
    EXPECT_EQ(reading.invariants[1].entries, 2U);
    EXPECT_EQ(reading.invariants[1].why, "kept by 2 entries");
    EXPECT_FALSE(reading.passed());
}

TEST(Invariants, AnEntryNeverScoredIsInconclusiveAndOneScoredWithoutTheMetricKeepsIt) {
    const Gate gate{.name = "g", .invariants = {Invariant{.metric = "quotes_kept", .every_entry = 1}}};
    const std::vector<ScoredEntry> nothing_to_keep{scored("lecture", 0, {code({at("atoms_told", 1.0)})})};
    const GateReading kept = read_gate_on(gate, {}, nothing_to_keep, kPanel);
    EXPECT_EQ(kept.invariants.front().outcome, Outcome::passed) << "no quote to keep: nothing to break";
    EXPECT_TRUE(kept.passed());

    const std::vector<ScoredEntry> unscored{scored("lecture", 0, {code({at("quotes_kept", 1.0)})}),
                                            scored("podcast", 2, {})};
    const GateReading reading = read_gate_on(gate, {}, unscored, kPanel);
    EXPECT_EQ(reading.invariants.front().outcome, Outcome::inconclusive);
    EXPECT_EQ(reading.invariants.front().why, "not scored by code: podcast, replicate 2");
    EXPECT_FALSE(reading.passed()) << "inconclusive is no pass";

    EXPECT_EQ(read_gate_on(gate, {}, {}, kPanel).invariants.front().why, "the candidate has no entry");
}

TEST(Invariants, OfAJudgedMetricCountOnceItsPanelIsCalibrated) {
    const Gate gate{.name = "g", .invariants = {Invariant{.metric = "contradicts", .every_entry = 0}}};
    const std::vector<ScoredEntry> candidate{scored("lecture", 0, {judges({at("contradicts", 0.5)})})};
    const InvariantReading shown = read_gate_on(gate, {}, candidate, kPanel).invariants.front();
    EXPECT_EQ(shown.outcome, Outcome::ignored);
    EXPECT_THAT(shown.why,
                HasSubstr("the panel eval-v9 judges it, and no person has calibrated the panel yet"));
    EXPECT_EQ(read_gate_on(gate, {}, candidate, kCalibrated).invariants.front().outcome, Outcome::failed);

    const std::vector<ScoredEntry> other_panel{
        scored("lecture", 0, {judges({at("contradicts", 0.0)}, "eval-v8")})};
    const InvariantReading elsewhere = read_gate_on(gate, {}, other_panel, kCalibrated).invariants.front();
    EXPECT_EQ(elsewhere.outcome, Outcome::inconclusive) << "the gate's panel never read it";
    EXPECT_EQ(elsewhere.why, "not scored by judges@eval-v9: lecture, replicate 0");
}

TEST(Gated, PassesWhenTheIntervalsWorseEndIsWithinTheMargin) {
    // Every video 0.005 worse: an interval of no width, within 0.01.
    const GatedReading reading = read_gate_on(kAtomsTold, two_videos("atoms_told", 0.900, 0.950),
                                              two_videos("atoms_told", 0.895, 0.945), kPanel)
                                     .gated.front();
    EXPECT_EQ(reading.outcome, Outcome::passed);
    EXPECT_EQ(reading.why, "candidate - baseline -0.005 to -0.005: within its margin");
}

TEST(Gated, FailsAsWorseWhenTheWholeIntervalIsPastItsMarginAndZero) {
    const GatedReading reading = read_gate_on(kAtomsTold, two_videos("atoms_told", 0.90, 0.95),
                                              two_videos("atoms_told", 0.88, 0.93), kPanel)
                                     .gated.front();
    EXPECT_EQ(reading.outcome, Outcome::failed);
    EXPECT_THAT(reading.why, HasSubstr("worse, past its margin, and wholly on the worse side of zero"));
}

TEST(Gated, IsInconclusiveWhenTheIntervalSpansItsMarginAndZero) {
    // One video 0.04 worse, the other 0.03 better: a resample may draw either
    // twice, so the interval runs from -0.04 to +0.03. Its middle, -0.005, is
    // within the margin: a gate reading the mean would pass it.
    const GateReading reading = read_gate_on(kAtomsTold, two_videos("atoms_told", 0.90, 0.90),
                                             two_videos("atoms_told", 0.86, 0.93), kPanel);
    EXPECT_EQ(reading.gated.front().outcome, Outcome::inconclusive);
    EXPECT_THAT(reading.gated.front().why,
                HasSubstr("spans both its margin and zero: add videos or replicates"));
    EXPECT_FALSE(reading.passed());
}

TEST(Gated, AMetricBetterLowerIsWorseUpward) {
    const Gate gate{.name = "g", .gated = {Gated{.metric = "contradicts", .margin = 0.01}}};
    const auto read = [&](double lecture, double podcast) {
        return read_gate_on(gate, two_videos("contradicts", 0.0, 0.0, "judges@eval-v9"),
                            two_videos("contradicts", lecture, podcast, "judges@eval-v9"), kCalibrated)
            .gated.front()
            .outcome;
    };
    EXPECT_EQ(read(0.005, 0.005), Outcome::passed) << "0.005 more, within 0.01";
    EXPECT_EQ(read(0.02, 0.02), Outcome::failed) << "0.02 more";
    EXPECT_EQ(read(-0.02, -0.02), Outcome::passed) << "fewer is better";
}

TEST(Gated, AJudgedMetricIsShownNotCountedUntilItsPanelIsCalibrated) {
    const Gate gate{.name = "g", .gated = {Gated{.metric = "facts_told", .margin = 0.01}}};
    const auto baseline = two_videos("facts_told", 0.9, 0.9, "judges@eval-v9");
    const auto worse = two_videos("facts_told", 0.5, 0.5, "judges@eval-v9");
    const GateReading shown = read_gate_on(gate, baseline, worse, kPanel);
    EXPECT_EQ(shown.gated.front().outcome, Outcome::ignored);
    EXPECT_THAT(shown.gated.front().why, HasSubstr("not counted (candidate - baseline -0.400 to -0.400)"));
    EXPECT_TRUE(shown.passed()) << "ignored counts for nothing";
    EXPECT_EQ(read_gate_on(gate, baseline, worse, kCalibrated).gated.front().outcome, Outcome::failed);

    const auto elsewhere = two_videos("facts_told", 0.9, 0.9, "judges@eval-v8");
    const GatedReading other = read_gate_on(gate, elsewhere, elsewhere, kCalibrated).gated.front();
    EXPECT_EQ(other.outcome, Outcome::inconclusive);
    EXPECT_EQ(other.why, "judged by judges@eval-v8, not by the gate's panel, eval-v9");
    const GatedReading unjudged = read_gate_on(gate, {}, {}, kCalibrated).gated.front();
    EXPECT_EQ(unjudged.why, "neither run was judged by the panel eval-v9: rreport eval judge");
}

TEST(Gated, IsInconclusiveWhatTheNumbersCannotSay) {
    const auto why = [](const std::vector<ScoredEntry>& baseline, const std::vector<ScoredEntry>& candidate) {
        const GatedReading reading = read_gate_on(kAtomsTold, baseline, candidate, kPanel).gated.front();
        EXPECT_EQ(reading.outcome, Outcome::inconclusive);
        return reading.why;
    };
    EXPECT_EQ(why({}, {}), "neither run was scored: rreport eval score");
    const std::vector<ScoredEntry> one{scored("lecture", 0, {code({at("atoms_told", 0.9)})})};
    EXPECT_EQ(why(one, one), "1 video scored by both runs: an interval needs two at least");
    EXPECT_THAT(why(two_videos("atoms_told", 0.9, 0.9), two_videos("atoms_told", 0.9, 0.9, "code@2")),
                HasSubstr("run A was scored by code@1 and run B by code@2"));
}

TEST(GateReading, IsWrittenOutAsTheGateCommandPrintsIt) {
    const Gate gate{.name = "g",
                    .invariants = {Invariant{.metric = "invalid_markers", .every_entry = 0}},
                    .gated = {Gated{.metric = "atoms_told", .margin = 0.01}}};
    const std::vector<ScoredEntry> baseline{
        scored("lecture", 0, {code({at("atoms_told", 0.9), at("invalid_markers", 0)})}),
        scored("podcast", 0, {code({at("atoms_told", 0.9), at("invalid_markers", 0)})})};
    std::vector<ScoredEntry> candidate = baseline;
    candidate[1].scorings[0].scores[1].value = 1;
    const std::string text = to_text(gate, read_gate_on(gate, baseline, candidate, kPanel), "A", "B");
    EXPECT_THAT(text, HasSubstr("gate g: candidate B against baseline A\n"));
    EXPECT_THAT(text,
                HasSubstr("  invalid_markers        at most 0.000    failed: broken by podcast, replicate 0: "
                          "1.000\n"));
    EXPECT_THAT(text,
                HasSubstr("  atoms_told             margin 0.010     passed: candidate - baseline +0.000 to "
                          "+0.000: within its margin\n"));
    EXPECT_THAT(text, HasSubstr("\ngate g: failed\n"));
}

EvalRun spec_run(const std::string& label, std::string tier, int first, int replicates) {
    return EvalRun{.id = label,
                   .spec = RunSpec{.label = label,
                                   .eval_set = "m2",
                                   .tier = std::move(tier),
                                   .replicates = replicates,
                                   .first_replicate = first}};
}

TEST(CheckAa, AnAaRunIsTheBaselineRunAgainUnderOtherSalts) {
    const EvalRun baseline = spec_run("base", "max", 0, 3);
    EXPECT_OK(check_aa(baseline, spec_run("base-aa", "max", 3, 3)));
    EXPECT_OK(check_aa(baseline, spec_run("base-aa", "max", 5, 2)));

    const auto shared = check_aa(baseline, spec_run("base-aa", "max", 2, 3));
    EXPECT_ERRC(shared, Errc::invalid_argument);
    EXPECT_THAT(shared.error().message(),
                HasSubstr("base-aa shares salts with base: its replicates 2 to 4 and 0 to 2 overlap"));
    EXPECT_THAT(shared.error().message(), HasSubstr("--first-replicate 3"));

    const auto other = check_aa(baseline, spec_run("fast", "fast", 3, 3));
    EXPECT_ERRC(other, Errc::invalid_argument);
    EXPECT_THAT(other.error().message(), HasSubstr("fast is not base run again"));
}

// Forty videos whose A/A differences are ±size, as entries of two runs.
std::pair<std::vector<ScoredEntry>, std::vector<ScoredEntry>> aa_runs(double size, double shift = 0) {
    std::pair<std::vector<ScoredEntry>, std::vector<ScoredEntry>> runs;
    for (int video = 0; video < 40; ++video) {
        const std::string name = std::format("v{:02}", video);
        runs.first.push_back(scored(name, 0, {code({at("atoms_told", 0.9)})}));
        runs.second.push_back(
            scored(name, 3, {code({at("atoms_told", 0.9 + shift + (video % 2 == 0 ? size : -size))})}));
    }
    return runs;
}

TEST(AaReading, TheMarginsHoldWhenAnUnchangedPipelinePassesThemFourTimesInFive) {
    const auto [baseline, aa] = aa_runs(0.01);
    const Gate wide{.name = "g", .gated = {Gated{.metric = "atoms_told", .margin = 0.02}}};
    const AaReading holding = read_gate_on_aa(wide, baseline, aa, kPanel);
    ASSERT_EQ(holding.metrics.size(), 1U);
    EXPECT_EQ(holding.metrics.front().outcome, Outcome::passed);
    EXPECT_GT(holding.metrics.front().alone.value_or(0), 0.99);
    EXPECT_GE(holding.unchanged_passes.value_or(0), kUnchangedPasses);
    EXPECT_TRUE(holding.holds());
    EXPECT_THAT(holding.off_zero, IsEmpty());
    EXPECT_THAT(to_text(wide, holding, "A", "A2"), HasSubstr("gate g: its margins hold\n"));

    const Gate tight{.name = "g", .gated = {Gated{.metric = "atoms_told", .margin = 0.001}}};
    const AaReading coin = read_gate_on_aa(tight, baseline, aa, kPanel);
    EXPECT_LT(coin.unchanged_passes.value_or(1), kUnchangedPasses);
    EXPECT_FALSE(coin.holds());
    EXPECT_THAT(to_text(tight, coin, "A", "A2"), HasSubstr("its margins do not hold"));
}

TEST(AaReading, ListsAnAaIntervalThatLeavesOutZero) {
    const auto [baseline, aa] = aa_runs(0.0, 0.004);
    const Gate gate{.name = "g", .gated = {Gated{.metric = "atoms_told", .margin = 0.02}}};
    const AaReading reading = read_gate_on_aa(gate, baseline, aa, kPanel);
    EXPECT_THAT(reading.off_zero, ElementsAre("atoms_told"));
    EXPECT_TRUE(reading.holds()) << "chance does that now and then: no failure by itself";
}

TEST(AaReading, CountsOnlyWhatTheGateCountsAndCannotHoldOnTooFewVideos) {
    const Gate gate{.name = "g", .gated = {Gated{.metric = "facts_told", .margin = 0.02}}};
    const AaReading ignored = read_gate_on_aa(gate, two_videos("facts_told", 0.9, 0.8, "judges@eval-v9"),
                                              two_videos("facts_told", 0.8, 0.9, "judges@eval-v9"), kPanel);
    EXPECT_EQ(ignored.metrics.front().outcome, Outcome::ignored);
    EXPECT_EQ(ignored.unchanged_passes, 1.0) << "nothing counted: nothing to fail";

    const std::vector<ScoredEntry> one{scored("lecture", 0, {code({at("atoms_told", 0.9)})})};
    const AaReading few = read_gate_on_aa(kAtomsTold, one, one, kPanel);
    EXPECT_EQ(few.metrics.front().outcome, Outcome::inconclusive);
    EXPECT_FALSE(few.holds());
}

}  // namespace
}  // namespace rr::eval

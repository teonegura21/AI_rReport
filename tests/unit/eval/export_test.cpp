// A run's scores exported: a row per video and replicate, its metrics in
// columns sorted by name, a metric an entry has not an empty cell; CSV's
// quoting; the same as JSON.
#include <rr/eval/export.hpp>

#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "fixtures.hpp"

namespace rr::eval {
namespace {

EvalRun run(std::string label = "baseline") {
    return EvalRun{.id = "01JRUN",
                   .spec = RunSpec{.label = std::move(label),
                                   .eval_set = "m2",
                                   .split = Split::dev,
                                   .tier = "max",
                                   .replicates = 2}};
}

EvalSet set() {
    return EvalSet{.name = "m2", .videos = {fixtures::video()}};
}

std::vector<ScoredEntry> entries() {
    return {ScoredEntry{.entry = RunEntry{.video = "radium-talk", .replicate = 0, .job = "01JA"},
                        .scorer = "code@1",
                        .scores = {Score{.metric = "minutes", .value = 12.5},
                                   Score{.metric = "atoms_told", .value = 0.96}}},
            ScoredEntry{.entry = RunEntry{.video = "radium-talk", .replicate = 1, .job = "01JB"},
                        .scorer = "code@1",
                        .scores = {Score{.metric = "minutes", .value = 13}}},
            ScoredEntry{.entry = RunEntry{.video = "gone-talk", .replicate = 0, .job = "01JC"}}};
}

TEST(Export, ACsvRowAnEntryItsMetricsInColumnsSortedByName) {
    EXPECT_EQ(to_csv(run(), set(), entries()),
              "run,tier,video,type,captions,replicate,job,scorer,atoms_told,minutes\r\n"
              "baseline,max,radium-talk,talk,manual,0,01JA,code@1,0.96,12.5\r\n"
              "baseline,max,radium-talk,talk,manual,1,01JB,code@1,,13\r\n"
              "baseline,max,gone-talk,,,0,01JC,,,\r\n");
}

TEST(Export, CsvQuotesWhatWouldBreakAField) {
    const std::string csv = to_csv(run(R"(thinking, "on")"), set(), entries());
    EXPECT_THAT(csv, ::testing::StartsWith("run,tier,"));
    EXPECT_THAT(csv, ::testing::HasSubstr("\r\n\"thinking, \"\"on\"\"\",max,radium-talk,"));
}

TEST(Export, JsonHoldsTheRunAndEachEntrysScores) {
    const json::Json exported = to_json(run(), set(), entries());
    EXPECT_EQ(
        exported.at("run"),
        (json::Json{
            {"label", "baseline"}, {"set", "m2"}, {"split", "dev"}, {"tier", "max"}, {"replicates", 2}}));
    ASSERT_EQ(exported.at("entries").size(), 3U);
    EXPECT_EQ(exported.at("entries")[0], (json::Json{{"video", "radium-talk"},
                                                     {"replicate", 0},
                                                     {"job", "01JA"},
                                                     {"type", "talk"},
                                                     {"captions", "manual"},
                                                     {"scorer", "code@1"},
                                                     {"scores", {{"atoms_told", 0.96}, {"minutes", 12.5}}}}));
    EXPECT_EQ(exported.at("entries")[2].at("type"), "") << "a video no longer in the set";
}

}  // namespace
}  // namespace rr::eval

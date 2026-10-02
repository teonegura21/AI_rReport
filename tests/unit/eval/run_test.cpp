// The eval's runs, on a real library: one label one run, taken up where it
// stopped; each video of the run's split once a replicate, replicate by
// replicate; an entry's job made from its snapshot when its turn comes, and
// kept; scores kept whole.
#include <rr/eval/run.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/core/clock.hpp>
#include <rr/core/error.hpp>
#include <rr/db/database.hpp>
#include <rr/eval/snapshot.hpp>
#include <rr/library/library.hpp>
#include <rr/pipeline/source.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

#include "fixtures.hpp"

namespace rr::eval {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using testkit::unwrap;

RunSpec spec(std::string label = "baseline") {
    return RunSpec{
        .label = std::move(label), .eval_set = "m2", .split = Split::dev, .tier = "max", .replicates = 2};
}

class RunsTest : public ::testing::Test {
protected:
    // A video of the set, its snapshot kept unless it is `unfetched`.
    [[nodiscard]] EvalVideo video(std::string id, Split split = Split::dev, bool unfetched = false) {
        EvalVideo made = fixtures::video();
        made.id = std::move(id);
        made.split = split;
        if (!unfetched) {
            const pipeline::Source source = fixtures::source_of(
                fixtures::vtt_of({{.text = "Radium glows.", .start = Millis{0}, .end = Millis{2000}}}));
            (void)unwrap(keep_snapshot(root(), made, pipeline::to_json(source), clock_.now()));
        }
        return made;
    }

    [[nodiscard]] std::filesystem::path root() const { return dir_ / "eval"; }
    [[nodiscard]] Runs& runs() { return runs_; }
    [[nodiscard]] library::Jobs& jobs() { return jobs_; }
    [[nodiscard]] FakeClock& clock() { return clock_; }

private:
    testkit::TempDir dir_;
    db::Database database_ = unwrap(library::open_database(dir_ / "library.db"));
    FakeClock clock_;
    Runs runs_{database_, clock_};
    library::Jobs jobs_{database_, clock_};
};

TEST_F(RunsTest, ALabelMakesARunOnceAndTakesItUpAfter) {
    const EvalRun made = unwrap(runs().open(spec()));
    EXPECT_EQ(made.spec, spec());
    EXPECT_EQ(made.id.size(), 26U) << "a ULID";
    EXPECT_EQ(unwrap(runs().open(spec())), made) << "the same run, taken up";
    EXPECT_EQ(unwrap(runs().find("baseline")), made);
    EXPECT_NE(unwrap(runs().open(spec("thinking-on"))).id, made.id) << "another label, another run";
}

TEST_F(RunsTest, AllAreTheRunsNewestFirst) {
    EXPECT_TRUE(unwrap(runs().all()).empty());
    const EvalRun first =
        unwrap(runs().open(RunSpec{.label = "first", .eval_set = "m2", .tier = "max", .replicates = 1}));
    clock().advance(std::chrono::seconds{1});
    const EvalRun second =
        unwrap(runs().open(RunSpec{.label = "second", .eval_set = "m2", .tier = "fast", .replicates = 2}));
    const std::vector<EvalRun> all = unwrap(runs().all());
    ASSERT_EQ(all.size(), 2U);
    EXPECT_EQ(all[0], second);
    EXPECT_EQ(all[1], first);
}

TEST_F(RunsTest, ALabelNamesOneRun) {
    (void)unwrap(runs().open(spec()));
    RunSpec other = spec();
    other.eval_set = "m3";
    other.split = Split::test;
    other.tier = "fast";
    other.replicates = 3;
    other.first_replicate = 3;
    const Result<EvalRun> refused = runs().open(other);
    EXPECT_ERRC(refused, Errc::conflict);
    EXPECT_THAT(
        refused.error().message(),
        HasSubstr("the run 'baseline' is of the set m2, not m3; the split dev, not test; the tier max, "
                  "not fast; replicates 2, not 3; the first replicate 0, not 3"));
}

TEST_F(RunsTest, ARunNeedsALabelAndOneReplicateAtLeast) {
    EXPECT_ERRC(runs().open(spec("")), Errc::invalid_argument);
    RunSpec none = spec();
    none.replicates = 0;
    const Result<EvalRun> refused = runs().open(none);
    EXPECT_ERRC(refused, Errc::invalid_argument);
    EXPECT_THAT(refused.error().message(), HasSubstr("takes each video once at least, not 0 times"))
        << "said by the code, before the table's own check";
    RunSpec before = spec();
    before.first_replicate = -1;
    const Result<EvalRun> negative = runs().open(before);
    EXPECT_ERRC(negative, Errc::invalid_argument);
    EXPECT_THAT(negative.error().message(), HasSubstr("replicates are numbered from 0, not -1"));
}

TEST_F(RunsTest, ARunNotMadeSaysHowToMakeIt) {
    const Result<EvalRun> missing = runs().find("nightly");
    EXPECT_ERRC(missing, Errc::not_found);
    EXPECT_THAT(missing.error().message(), HasSubstr("`rreport eval run --label nightly` makes it"));
}

TEST_F(RunsTest, EachVideoOfTheSplitIsPlannedOnceAReplicateReplicateByReplicate) {
    const std::vector<EvalVideo> videos{video("b-talk"), video("a-talk"), video("held-out", Split::test),
                                        video("unfetched", Split::dev, true)};
    const EvalRun run = unwrap(runs().open(spec()));
    const Planned planned = unwrap(plan_run(root(), run, videos, runs()));
    EXPECT_THAT(
        planned.entries,
        ElementsAre(RunEntry{.video = "b-talk", .replicate = 0}, RunEntry{.video = "a-talk", .replicate = 0},
                    RunEntry{.video = "b-talk", .replicate = 1}, RunEntry{.video = "a-talk", .replicate = 1}))
        << "every video once before any twice, in the set's order; none of the test split";
    EXPECT_THAT(planned.unfetched, ElementsAre("unfetched"));
}

TEST_F(RunsTest, ARunsReplicatesAreNumberedFromItsFirstAsAnAaRunsAre) {
    const std::vector<EvalVideo> videos{video("a-talk")};
    RunSpec again = spec("baseline-aa");
    again.first_replicate = 2;
    const EvalRun run = unwrap(runs().open(again));
    EXPECT_EQ(run.spec.first_replicate, 2);
    EXPECT_EQ(unwrap(runs().find("baseline-aa")).spec.first_replicate, 2) << "kept";
    EXPECT_THAT(
        unwrap(plan_run(root(), run, videos, runs())).entries,
        ElementsAre(RunEntry{.video = "a-talk", .replicate = 2}, RunEntry{.video = "a-talk", .replicate = 3}))
        << "salts 2 and 3, none the baseline's 0 and 1";
}

TEST_F(RunsTest, AnEntryGetsAnArticleJobFromItsSnapshotWhenItsTurnComesAndKeepsIt) {
    const std::vector<EvalVideo> videos{video("a-talk")};
    const EvalRun run = unwrap(runs().open(spec()));
    Planned planned = unwrap(plan_run(root(), run, videos, runs()));
    EXPECT_THAT(unwrap(runs().entries(run.id)), IsEmpty()) << "planning makes no job";

    RunEntry& second = planned.entries.at(1);
    const library::Job job = unwrap(start_entry(root(), run, second, runs(), jobs()));
    EXPECT_EQ(job.source, std::filesystem::absolute(snapshot_file(root(), "a-talk")).string());
    EXPECT_EQ(job.tier, "max");
    EXPECT_EQ(job.salt, 1) << "the replicate";
    EXPECT_EQ(job.kind, library::JobKind::article);
    EXPECT_EQ(second.job, job.id);
    EXPECT_THAT(unwrap(runs().entries(run.id)),
                ElementsAre(RunEntry{.video = "a-talk", .replicate = 1, .job = job.id}));

    Planned again = unwrap(plan_run(root(), run, videos, runs()));
    EXPECT_EQ(again.entries.at(0).job, "");
    EXPECT_EQ(again.entries.at(1).job, job.id) << "taken up: the job it made";
    EXPECT_EQ(unwrap(start_entry(root(), run, again.entries.at(1), runs(), jobs())).id, job.id);

    const library::Job other = unwrap(jobs().create("elsewhere.vtt", "max", 1));
    EXPECT_ERRC(runs().add(run.id, RunEntry{.video = "a-talk", .replicate = 1, .job = other.id}),
                Errc::conflict)
        << "one job a replicate";
}

TEST_F(RunsTest, AnEntryScoredAgainByAFamilyIsScoredWholeByItAndOtherFamiliesStay) {
    const EvalRun run = unwrap(runs().open(spec()));
    const library::Job job = unwrap(jobs().create("talk.vtt", "max", 0));
    const RunEntry entry{.video = "a-talk", .replicate = 0, .job = job.id};
    unwrap(runs().add(run.id, entry));
    const Scoring first{
        .scorer = "code@1",
        .scores = {{.metric = "atoms_told", .value = 0.5}, {.metric = "minutes", .value = 3}}};
    unwrap(runs().keep(run.id, entry, first));
    const Scoring judged{.scorer = "judges@eval-v1", .scores = {{.metric = "facts_told", .value = 0.9}}};
    unwrap(runs().keep(run.id, entry, judged));
    const Scoring second{.scorer = "code@2", .scores = {{.metric = "atoms_told", .value = 1.0}}};
    unwrap(runs().keep(run.id, entry, second));
    EXPECT_THAT(unwrap(runs().scores(run.id)),
                ElementsAre(ScoredEntry{.entry = entry, .scorings = {second, judged}}))
        << "code@1's minutes gone with it; the judges' scores kept";

    const Scoring rejudged{.scorer = "judges@eval-v2", .scores = {}};
    unwrap(runs().keep(run.id, entry, rejudged));
    EXPECT_THAT(unwrap(runs().scores(run.id)), ElementsAre(ScoredEntry{.entry = entry, .scorings = {second}}))
        << "a scoring of nothing still replaces its family's";

    EXPECT_ERRC(runs().keep(run.id, entry, Scoring{.scorer = "code"}), Errc::invalid_argument);
    EXPECT_ERRC(
        runs().keep(run.id, entry,
                    Scoring{.scorer = "judges@eval-v1", .scores = {{.metric = "atoms_told", .value = 1.0}}}),
        Errc::conflict)
        << "a metric is one family's";
}

TEST(ScorerFamily, IsWhatComesBeforeTheAt) {
    EXPECT_EQ(scorer_family("judges@eval-v1"), "judges");
    EXPECT_EQ(scorer_family("code@1"), "code");
    EXPECT_EQ(scorer_family("code"), std::nullopt);
    EXPECT_EQ(scorer_family("@1"), std::nullopt);
    EXPECT_EQ(scorer_family("code@"), std::nullopt);
}

TEST_F(RunsTest, EveryEntryIsListedWithItsScoresOrNone) {
    const EvalRun run = unwrap(runs().open(spec()));
    const RunEntry scored{.video = "b-talk", .replicate = 0, .job = unwrap(jobs().create("b", "max", 0)).id};
    const RunEntry waiting{.video = "a-talk", .replicate = 1, .job = unwrap(jobs().create("a", "max", 1)).id};
    unwrap(runs().add(run.id, scored));
    unwrap(runs().add(run.id, waiting));
    const Scoring scoring{
        .scorer = "code@1",
        .scores = {{.metric = "atoms_told", .value = 1}, {.metric = "minutes", .value = 2}}};
    unwrap(runs().keep(run.id, scored, scoring));
    EXPECT_THAT(
        unwrap(runs().scores(run.id)),
        ElementsAre(ScoredEntry{.entry = waiting}, ScoredEntry{.entry = scored, .scorings = {scoring}}))
        << "by video, then replicate";
    EXPECT_THAT(unwrap(runs().entries(run.id)), ElementsAre(waiting, scored))
        << "the same order, not the order made";
}

}  // namespace
}  // namespace rr::eval

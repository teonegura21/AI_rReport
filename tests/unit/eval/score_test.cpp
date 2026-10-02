// An article measured by code alone, on the stages' synthetic lecture
// (tests/unit/pipeline/lecture.hpp): the Fidelity Report's figures exactly
// as the page shows them; gold a person reviewed, scored against what a
// reader reads; a share with nothing to share left out; what the job cost,
// from its call log and battle log; a run scored on a real library.
#include <rr/eval/score.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/core/clock.hpp>
#include <rr/core/file_io.hpp>
#include <rr/core/strings.hpp>
#include <rr/db/database.hpp>
#include <rr/eval/snapshot.hpp>
#include <rr/knowledge/article.hpp>
#include <rr/library/library.hpp>
#include <rr/library/stage_store.hpp>
#include <rr/pipeline/atomize_stage.hpp>
#include <rr/pipeline/outline_stage.hpp>
#include <rr/pipeline/plan_stage.hpp>
#include <rr/pipeline/seams_stage.hpp>
#include <rr/pipeline/transcript_stage.hpp>
#include <rr/pipeline/write_stage.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

#include "../pipeline/lecture.hpp"
#include "fixtures.hpp"

namespace rr::eval {
namespace {

using namespace std::chrono_literals;
using knowledge::SentenceId;
using ::testing::DoubleEq;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Optional;
using testkit::unwrap;

// The lecture's job, done: its artifacts as the oracle makes them, and its
// article as the oracle writes it. Fifteen minutes long.
report::Contents lecture_contents() {
    report::Contents contents{.job = "J1", .tier = "max"};
    contents.video = pipeline::testing::source().video;
    contents.video->duration = 15min;
    contents.transcript = pipeline::testing::lecture();
    contents.outline = pipeline::testing::lecture_outline();
    contents.ledger = pipeline::testing::lecture_ledger();
    contents.plan = pipeline::testing::lecture_plan();
    contents.article = knowledge::Article{.markdown = pipeline::testing::oracle_article()};
    return contents;
}

// The article with `told` taken out of it.
report::Contents without(std::string_view told) {
    report::Contents contents = lecture_contents();
    contents.article->markdown = replace_all(contents.article->markdown, told, "");
    return contents;
}

// The score of `metric`, or none.
std::optional<double> score(const std::vector<Score>& scores, std::string_view metric) {
    const auto found = std::ranges::find(scores, metric, &Score::metric);
    return found == scores.end() ? std::nullopt : std::optional{found->value};
}

// The metrics `scores` has, in order.
std::vector<std::string> metrics(const std::vector<Score>& scores) {
    std::vector<std::string> names;
    names.reserve(scores.size());
    for (const Score& scored : scores) {
        names.push_back(scored.metric);
    }
    return names;
}

// A stretch of the lecture: sentence `n` (from 1) is said from 30 s × (n − 1),
// for 25 s.
Millis said_from(int sentence) {
    return Millis{(sentence - 1) * 30'000};
}
Millis said_until(int sentence) {
    return said_from(sentence) + 25s;
}

// Gold for the lecture: three facts, two of them core; two must-quotes, one
// the article keeps; two terms, one it uses.
Gold lecture_gold() {
    return Gold{
        .video = "radium-talk",
        .snapshot = "pinned",
        .status = GoldStatus::gold,
        .reviewer = "Ada",
        .facts = {Fact{.id = "F1",
                       .start = said_from(4),
                       .end = said_until(4),
                       .importance = Importance::core,
                       .quote = "The NRC lent us 3 scales",
                       .claim = "The NRC lent them three scales."},
                  Fact{.id = "F2",
                       .start = said_from(7),
                       .end = said_until(8),
                       .importance = Importance::detail,
                       .quote = "the weight it held doubled",
                       .claim = "A second pan doubled the weight held."},
                  Fact{.id = "F3",
                       .start = said_from(10),
                       .end = said_until(10),
                       .importance = Importance::core,
                       .quote = "Why does it glow",
                       .claim = "Ada asks why radium glows."}},
        .must_quotes = {MustQuote{
                            .start = said_from(9), .end = said_until(9), .quote = "the most precious thing"},
                        MustQuote{
                            .start = said_from(9), .end = said_until(9), .quote = "a glow like no other"}},
        .terms = {Term{.term = "NRC"}, Term{.term = "proof load"}}};
}

constexpr Costs kCosts{
    .elapsed = 30min, .calls = 19, .prompt_tokens = 4000, .completion_tokens = 900, .escalations = 2};

TEST(Score, TheFidelityReportsFiguresAreScoredAsThePageShowsThem) {
    const report::Contents contents = lecture_contents();
    const std::vector<Score> scores = scores_of(contents, nullptr, kCosts);
    const fidelity::Coverage kept = report::checked(contents)->coverage;
    EXPECT_THAT(score(scores, "sentences_accounted"),
                Optional(DoubleEq(static_cast<double>(kept.sentences_accounted) /
                                  static_cast<double>(kept.sentences))));
    EXPECT_THAT(score(scores, "sentences_accounted"), Optional(DoubleEq(1.0)));
    EXPECT_THAT(score(scores, "atoms_told"), Optional(DoubleEq(1.0)));
    EXPECT_THAT(score(scores, "invalid_markers"), Optional(DoubleEq(0.0)));
    EXPECT_THAT(score(scores, "quotes_kept"), Optional(DoubleEq(1.0)));
    EXPECT_THAT(score(scores, "numbers_kept"), Optional(DoubleEq(1.0)));
    EXPECT_THAT(metrics(scores),
                ElementsAre("sentences_accounted", "atoms_told", "invalid_markers", "quotes_kept",
                            "numbers_kept", "length_ratio", "minutes", "minutes_per_video_hour", "calls",
                            "prompt_tokens", "completion_tokens", "escalations"))
        << "no gold, no gold score";
}

TEST(Score, AnAtomLeftOutAndABadMarkerAreCountedAtOnce) {
    const std::vector<Score> scores = scores_of(without("Its half-life is long. [A4] "), nullptr, kCosts);
    EXPECT_THAT(score(scores, "atoms_told"), Optional(DoubleEq(25.0 / 26.0)));
    report::Contents marked = lecture_contents();
    marked.article->markdown = replace_all(marked.article->markdown, "[A4]", "[A4][A99]");
    EXPECT_THAT(score(scores_of(marked, nullptr, kCosts), "invalid_markers"), Optional(DoubleEq(1.0)));
}

TEST(Score, GoldIsScoredAgainstWhatAReaderReads) {
    const Gold gold = lecture_gold();
    const std::vector<Score> scores = scores_of(lecture_contents(), &gold, kCosts);
    EXPECT_THAT(score(scores, "gold_quotes_kept"), Optional(DoubleEq(0.5)));
    EXPECT_THAT(score(scores, "gold_terms_used"), Optional(DoubleEq(0.5)));
    EXPECT_THAT(score(scores, "facts_cited"), Optional(DoubleEq(1.0)));
    EXPECT_THAT(score(scores, "core_facts_cited"), Optional(DoubleEq(1.0)));

    const std::vector<Score> uncited = scores_of(without("Why does it glow? [A7]"), &gold, kCosts);
    EXPECT_THAT(score(uncited, "facts_cited"), Optional(DoubleEq(2.0 / 3.0)))
        << "F3's sentence's atom is not cited";
    EXPECT_THAT(score(uncited, "core_facts_cited"), Optional(DoubleEq(0.5)));
}

TEST(Score, AShareWithNothingToShareIsNoScore) {
    Gold gold = lecture_gold();
    gold.must_quotes.clear();
    gold.terms.clear();
    gold.facts.erase(gold.facts.begin());  // F1, core
    gold.facts.pop_back();                 // F3, core
    const std::vector<Score> scores = scores_of(lecture_contents(), &gold, kCosts);
    EXPECT_EQ(score(scores, "gold_quotes_kept"), std::nullopt);
    EXPECT_EQ(score(scores, "gold_terms_used"), std::nullopt);
    EXPECT_EQ(score(scores, "core_facts_cited"), std::nullopt) << "no core fact: not a 100%";
    EXPECT_THAT(score(scores, "facts_cited"), Optional(DoubleEq(1.0)));
}

TEST(Score, TheLengthIsTheArticlesWordsOverTheTranscripts) {
    report::Contents contents;
    contents.transcript =
        pipeline::testing::transcript_of({"Radium glows green.", "Marie Curie measured it."});
    contents.ledger = knowledge::AtomLedger{.atoms = {knowledge::Atom{.id = knowledge::AtomId{1},
                                                                      .text = "Radium glows green.",
                                                                      .sentences = {SentenceId{1}}},
                                                      knowledge::Atom{.id = knowledge::AtomId{2},
                                                                      .text = "Marie Curie measured it.",
                                                                      .sentences = {SentenceId{2}}}}};
    contents.article =
        knowledge::Article{.markdown =
                               "## Radium\n\nRadium glows green. [A1] Marie Curie measured it. [A2]\n\n"
                               "Then she said `nothing`. [A2]\n"};
    const std::vector<Score> scores = scores_of(contents, nullptr, kCosts);
    EXPECT_THAT(score(scores, "length_ratio"), Optional(DoubleEq(11.0 / 7.0)))
        << "the heading and the markers are no words; code within prose is";
}

TEST(Score, WhatTheJobCostIsScoredWithTheVideosLength) {
    const std::vector<Score> scores = scores_of(lecture_contents(), nullptr, kCosts);
    EXPECT_THAT(score(scores, "minutes"), Optional(DoubleEq(30.0)));
    EXPECT_THAT(score(scores, "minutes_per_video_hour"), Optional(DoubleEq(120.0)));
    EXPECT_THAT(score(scores, "calls"), Optional(DoubleEq(19.0)));
    EXPECT_THAT(score(scores, "prompt_tokens"), Optional(DoubleEq(4000.0)));
    EXPECT_THAT(score(scores, "completion_tokens"), Optional(DoubleEq(900.0)));
    EXPECT_THAT(score(scores, "escalations"), Optional(DoubleEq(2.0)));

    report::Contents unknown = lecture_contents();
    unknown.video->duration.reset();
    EXPECT_THAT(score(scores_of(unknown, nullptr, kCosts), "minutes_per_video_hour"),
                Optional(DoubleEq(30.0 / (895.0 / 3600.0))))
        << "without its length, where its transcript ends: S30 at 14:55";
}

TEST(Score, NoArticleIsNothingToScore) {
    report::Contents contents = lecture_contents();
    contents.article.reset();
    EXPECT_THAT(scores_of(contents, nullptr, kCosts), IsEmpty());
}

TEST(SentencesSaying, AreThoseItsAlignmentCoversWithinItsStretch) {
    const knowledge::Transcript lecture = pipeline::testing::lecture();
    EXPECT_THAT(sentences_saying("The NRC lent us 3 scales", said_from(4), said_until(4), lecture),
                ElementsAre(SentenceId{4}));
    EXPECT_THAT(sentences_saying("the weight it held doubled", said_from(7), said_until(8), lecture),
                ElementsAre(SentenceId{7}, SentenceId{8}))
        << "a quote across two sentences";
    EXPECT_THAT(sentences_saying("The NRC lent us 3 scales", said_from(4), said_until(5), lecture),
                ElementsAre(SentenceId{4}))
        << "of a stretch of two sentences, the one saying it";
    EXPECT_THAT(sentences_saying("the NRC lent us 3 scales", said_from(4) + 5s, said_until(4) - 5s, lecture),
                ElementsAre(SentenceId{4}))
        << "a stretch inside the sentence";
    EXPECT_THAT(sentences_saying("The NRC lent us 3 scales", said_until(4) + 1s, said_until(4) + 1s, lecture),
                ElementsAre(SentenceId{4}))
        << "a second after it: within two seconds";
    EXPECT_THAT(sentences_saying("The NRC lent us 3 scales", said_until(4) + 3s, said_until(4) + 3s, lecture),
                IsEmpty())
        << "three seconds after it: beyond them";
    EXPECT_THAT(sentences_saying("The NRC lent us 3 scales", said_from(6), said_until(6), lecture), IsEmpty())
        << "said, but not there";
    EXPECT_THAT(sentences_saying("The NRC lent us 4 scales", said_from(4), said_until(4), lecture), IsEmpty())
        << "not said";
}

class Scoring : public ::testing::Test {
protected:
    // A job done on the lecture: each stage's artifact committed, as a job
    // done has them.
    [[nodiscard]] library::Job done_job() {
        library::Job job = unwrap(jobs_.create(snapshot_file(root(), "radium-talk").string(), "max", 0));
        const auto commit = [&](std::string_view stage, json::Json artifact) {
            clock_.advance(1s);
            (void)unwrap(store_.commit(library::StageResult{.job_id = job.id,
                                                            .stage = std::string{stage},
                                                            .version = 1,
                                                            .key = job.id + std::string{stage},
                                                            .schema = "artifacts/x",
                                                            .artifact = std::move(artifact),
                                                            .started_at = clock_.now()}));
        };
        commit(pipeline::SourceStage::kName, pipeline::to_json(pipeline::testing::source()));
        commit(pipeline::TranscriptStage::kName, knowledge::to_json(pipeline::testing::lecture()));
        commit(pipeline::OutlineStage::kName, knowledge::to_json(pipeline::testing::lecture_outline()));
        commit(pipeline::AtomizeStage::kName, knowledge::to_json(pipeline::testing::lecture_ledger()));
        commit(pipeline::PlanStage::kName, knowledge::to_json(pipeline::testing::lecture_plan()));
        commit(pipeline::SeamsStage::kName,
               knowledge::to_json(knowledge::Article{.markdown = pipeline::testing::oracle_article()}));
        job.status = library::JobStatus::done;
        job.elapsed = 30min;
        return unwrap(jobs_.save(job));
    }

    // The video, its snapshot kept; its hash.
    std::string keep_video() {
        const pipeline::Source source = fixtures::source_of(
            fixtures::vtt_of({{.text = "Radium glows.", .start = Millis{0}, .end = Millis{2000}}}),
            std::nullopt);
        return unwrap(keep_snapshot(root(), fixtures::video(), pipeline::to_json(source), clock_.now())).hash;
    }

    void write_gold(const Gold& gold) {
        const std::filesystem::path file = gold_file(root(), gold.video);
        std::filesystem::create_directories(file.parent_path());
        (void)unwrap(write_file_atomically(file, to_toml(gold)));
    }

    // A model call of `job`, and a mission that answered at `step`.
    void call(const library::Job& job, std::string_view status, int prompt, int completion) {
        unwrap(database_.write([&](db::WriteTx& tx) -> Result<void> {
            RR_TRY(tx.execute(
                "INSERT INTO llm_calls (id, started_at_ms, duration_ms, provider, model, purpose, job_id, "
                "request_key, status, error, prompt_tokens, completion_tokens) VALUES (?1, 0, 10, "
                "'runtime', 'qwen', 'write', ?2, 'k', ?3, ?4, ?5, ?6)",
                {std::format("C{}", ++calls_), job.id, status,
                 status == "ok" ? db::Value{} : db::Value{std::string{"down"}}, prompt, completion}));
            return ok();
        }));
    }
    void mission(const library::Job& job, std::optional<std::string_view> step) {
        unwrap(database_.write([&](db::WriteTx& tx) -> Result<void> {
            RR_TRY(tx.execute(
                "INSERT INTO missions (job_id, mission, kind, role, started_at_ms, duration_ms, status, "
                "verdict, step, rungs, samples, accepted, calls) VALUES (?1, ?2, 'write', 'commander', "
                "0, 10, 'ok', 'accepted', ?3, ?4, 1, 1, 1)",
                {job.id, std::format("M{}", ++missions_),
                 step.has_value() ? db::Value{std::string{*step}} : db::Value{}, step.has_value() ? 1 : 0}));
            return ok();
        }));
    }

    [[nodiscard]] std::filesystem::path root() const { return dir_ / "eval"; }
    [[nodiscard]] db::Database& database() { return database_; }
    [[nodiscard]] const Clock& clock() const { return clock_; }
    [[nodiscard]] library::Jobs& jobs() { return jobs_; }
    [[nodiscard]] Runs& runs() { return runs_; }

private:
    testkit::TempDir dir_;
    db::Database database_ = unwrap(library::open_database(dir_ / "library.db"));
    FakeClock clock_;
    library::Jobs jobs_{database_, clock_};
    library::StageStore store_{database_, clock_};
    Runs runs_{database_, clock_};
    int calls_ = 0;
    int missions_ = 0;
};

TEST_F(Scoring, CostsAreEveryAttemptsTokensAndTheMissionsPastTheirFirstRung) {
    const library::Job job = done_job();
    call(job, "ok", 1000, 200);
    call(job, "unavailable", 400, 0);
    call(job, "ok", 600, 100);
    mission(job, "as_ordered");
    mission(job, "thinking");
    mission(job, "other_family");
    mission(job, std::nullopt);  // a ceiling before any rung
    EXPECT_EQ(unwrap(costs_of(database(), job)), (Costs{.elapsed = 30min,
                                                        .calls = 2,
                                                        .prompt_tokens = 2000,
                                                        .completion_tokens = 300,
                                                        .escalations = 2}));
}

TEST_F(Scoring, ARunIsScoredWhereItsJobsAreDoneAgainstReviewedGoldOfTheirSnapshot) {
    const std::string snapshot = keep_video();
    Gold gold = lecture_gold();
    gold.snapshot = snapshot;
    write_gold(gold);
    const EvalSet set{.name = "m2", .videos = {fixtures::video()}};
    const EvalRun run = unwrap(runs().open(
        RunSpec{.label = "baseline", .eval_set = "m2", .split = Split::dev, .tier = "max", .replicates = 2}));
    const library::Job done = done_job();
    const library::Job running = unwrap(jobs().create("talk.vtt", "max", 1));
    unwrap(runs().add(run.id, RunEntry{.video = "radium-talk", .replicate = 0, .job = done.id}));
    unwrap(runs().add(run.id, RunEntry{.video = "radium-talk", .replicate = 1, .job = running.id}));

    const RunScoring scoring = unwrap(score_run(root(), set, run, runs(), database(), clock()));
    EXPECT_EQ(scoring.scored, 1U);
    EXPECT_THAT(scoring.waiting,
                ElementsAre(std::format("radium-talk, replicate 1: job {} is running", running.id)));
    EXPECT_THAT(scoring.notes, IsEmpty());
    const std::vector<ScoredEntry> kept = unwrap(runs().scores(run.id));
    ASSERT_EQ(kept.size(), 2U);
    ASSERT_EQ(kept[0].scorings.size(), 1U);
    EXPECT_EQ(kept[0].scorings[0].scorer, kCodeScorer);
    EXPECT_THAT(score(kept[0].scorings[0].scores, "facts_cited"), Optional(DoubleEq(1.0)))
        << "scored against its gold";
    EXPECT_THAT(score(kept[0].scorings[0].scores, "minutes"), Optional(DoubleEq(30.0)));
    EXPECT_THAT(kept[1].scorings, IsEmpty()) << "not done: not scored";
}

TEST_F(Scoring, GoldNotReviewedOrOfAnotherSnapshotIsNotScoredAgainst) {
    const std::string snapshot = keep_video();
    const EvalSet set{.name = "m2", .videos = {fixtures::video()}};
    const EvalRun run = unwrap(runs().open(
        RunSpec{.label = "baseline", .eval_set = "m2", .split = Split::dev, .tier = "max", .replicates = 1}));
    unwrap(runs().add(run.id, RunEntry{.video = "radium-talk", .replicate = 0, .job = done_job().id}));
    const auto scored_with = [&]() {
        const RunScoring scoring = unwrap(score_run(root(), set, run, runs(), database(), clock()));
        const std::vector<ScoredEntry> kept = unwrap(runs().scores(run.id));
        return std::pair{scoring.notes, score(kept.at(0).scorings.at(0).scores, "facts_cited").has_value()};
    };
    EXPECT_EQ(scored_with(),
              (std::pair{std::vector<std::string>{"radium-talk: no gold yet: scored without it"}, false}));

    Gold gold = lecture_gold();
    gold.snapshot = snapshot;
    gold.status = GoldStatus::silver;
    write_gold(gold);
    EXPECT_EQ(scored_with(),
              (std::pair{std::vector<std::string>{
                             "radium-talk: its gold is silver, not reviewed yet: scored without it"},
                         false}));

    gold.status = GoldStatus::gold;
    gold.snapshot = "another";
    write_gold(gold);
    const auto [notes, against_gold] = scored_with();
    EXPECT_THAT(notes, ElementsAre(HasSubstr("its gold was written for another snapshot than the one read")));
    EXPECT_FALSE(against_gold);
}

}  // namespace
}  // namespace rr::eval

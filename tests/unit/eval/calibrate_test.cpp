// The panel's readings on a real library, as a person labels them and as
// calibration compares them: a judged article of the lecture and a pairing
// of two, their artifacts as the jobs left them.
#include <rr/eval/calibrate.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/core/clock.hpp>
#include <rr/db/database.hpp>
#include <rr/library/jobs.hpp>
#include <rr/library/library.hpp>
#include <rr/library/stage_store.hpp>
#include <rr/pipeline/atomize_stage.hpp>
#include <rr/pipeline/seams_stage.hpp>
#include <rr/pipeline/source.hpp>
#include <rr/pipeline/transcript_stage.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

#include "../pipeline/lecture.hpp"
#include "fixtures.hpp"

namespace rr::eval {
namespace {

using namespace std::chrono_literals;
using fixtures::lecture_facts;
using fixtures::panel;
using fixtures::prompts;
using ::testing::Each;
using ::testing::Field;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;
using testkit::unwrap;

class Calibrating : public ::testing::Test {
protected:
    // An article job done, its article `markdown`, of the lecture.
    [[nodiscard]] library::Job done_article(
        const std::string& markdown = pipeline::testing::oracle_article()) {
        library::Job job = unwrap(jobs_.create("talk.vtt", "max", 0));
        const auto commit = [&](std::string_view stage, json::Json artifact) {
            clock_.advance(1s);
            std::string key = std::format("{} {}", stage, json::content_hash(artifact).hex());
            (void)unwrap(store_.commit(library::StageResult{.job_id = job.id,
                                                            .stage = std::string{stage},
                                                            .version = 1,
                                                            .key = std::move(key),
                                                            .schema = "artifacts/x",
                                                            .artifact = std::move(artifact),
                                                            .started_at = clock_.now()}));
        };
        commit(pipeline::SourceStage::kName, pipeline::to_json(pipeline::testing::source()));
        commit(pipeline::TranscriptStage::kName, knowledge::to_json(pipeline::testing::lecture()));
        commit(pipeline::AtomizeStage::kName, knowledge::to_json(pipeline::testing::lecture_ledger()));
        commit(pipeline::SeamsStage::kName, knowledge::to_json(knowledge::Article{.markdown = markdown}));
        job.status = library::JobStatus::done;
        job.title = "Weighing radium";
        return unwrap(jobs_.save(job));
    }

    void judge(fixtures::Rig& rig, const library::Job& article) {
        ArticleToJudge read = unwrap(article_to_judge(article, store_, lecture_facts()));
        library::Job job = unwrap(start_judgement(database_, clock_, article, panel()));
        const JudgementRun run = unwrap(run_judgement(
            std::move(job), panel(), std::move(read), database_, clock_,
            pipeline::StageContext{
                .army = rig.raise(), .prompts = &prompts(), .work_directory = dir_.path() / "jobs"}));
        ASSERT_EQ(run.run.job.status, library::JobStatus::done);
    }

    void pair(fixtures::Rig& rig, const library::Job& a, const library::Job& b) {
        ArticlesToCompare read = unwrap(articles_to_compare(a, b, store_));
        library::Job job = unwrap(start_pairing(database_, clock_, a, b, panel()));
        const PairingRun run = unwrap(run_pairing(
            std::move(job), panel(), std::move(read), database_, clock_,
            pipeline::StageContext{
                .army = rig.raise(), .prompts = &prompts(), .work_directory = dir_.path() / "jobs"}));
        ASSERT_EQ(run.run.job.status, library::JobStatus::done);
    }

    [[nodiscard]] Result<LabelPool> pool(std::span<const RunEntry> entries, std::span<const VideoPairs> pairs,
                                         std::string_view name = "r5") {
        return label_pool(database_, clock_, FactsByVideo{{"lecture", lecture_facts()}}, entries, pairs,
                          panel(), name);
    }

    // `a` and `b` paired under the panel, its job done with `pairing` as
    // its artifact.
    void done_pairing(const library::Job& a, const library::Job& b, const Pairing& pairing) {
        library::Job job = unwrap(start_pairing(database_, clock_, a, b, panel()));
        clock_.advance(1s);
        (void)unwrap(store_.commit(library::StageResult{.job_id = job.id,
                                                        .stage = std::string{FlowStage::kName},
                                                        .version = 1,
                                                        .key = "a pairing written by the test",
                                                        .schema = "artifacts/pairing",
                                                        .artifact = to_json(pairing),
                                                        .started_at = clock_.now()}));
        job.status = library::JobStatus::done;
        (void)unwrap(jobs_.save(job));
    }

    // `article` judged, its job done with `judgement` as its artifact.
    void done_judgement(const library::Job& article, const Judgement& judgement) {
        library::Job job = unwrap(start_judgement(database_, clock_, article, panel()));
        clock_.advance(1s);
        (void)unwrap(store_.commit(library::StageResult{.job_id = job.id,
                                                        .stage = std::string{JudgementStage::kName},
                                                        .version = 1,
                                                        .key = "a judgement written by the test",
                                                        .schema = "artifacts/judgement",
                                                        .artifact = to_json(judgement),
                                                        .started_at = clock_.now()}));
        job.status = library::JobStatus::done;
        (void)unwrap(jobs_.save(job));
    }

    // A judgement started, never run.
    void judgement_begun(const library::Job& article) {
        (void)unwrap(start_judgement(database_, clock_, article, panel()));
    }

    [[nodiscard]] const library::StageStore& store() const { return store_; }
    [[nodiscard]] db::Database& database() { return database_; }
    [[nodiscard]] FakeClock& clock() { return clock_; }

private:
    testkit::TempDir dir_;
    db::Database database_ = unwrap(library::open_database(dir_.path() / "library.db"));
    FakeClock clock_;
    library::Jobs jobs_{database_, clock_};
    library::StageStore store_{database_, clock_};
};

// The oracle's article of the lecture, its question asked in fewer words:
// both judges prefer it, in both orders.
std::string shorter_question() {
    std::string markdown = pipeline::testing::oracle_article();
    const std::string asked = "Why does it glow?";
    const std::size_t at = markdown.find(asked);
    EXPECT_NE(at, std::string::npos);
    return markdown.replace(at, asked.size(), "Why?");
}

RunEntry entry_of(const library::Job& job, int replicate = 0) {
    return RunEntry{.video = "lecture", .replicate = replicate, .job = job.id};
}

TEST_F(Calibrating, ALabelShowsWhatTheJudgesWereShownNeverWhatTheySaid) {
    fixtures::Rig rig{{}, fixtures::engines_serving()};
    const library::Job a = done_article();
    const library::Job b = done_article(shorter_question());
    judge(rig, a);
    pair(rig, a, b);
    const std::vector<RunEntry> entries{entry_of(a)};
    const std::vector<VideoPairs> pairs{
        VideoPairs{.video = "lecture", .pairs = {{entry_of(a), entry_of(b)}}}};
    const LabelPool found = unwrap(pool(entries, pairs));
    EXPECT_THAT(found.notes, IsEmpty());

    ASSERT_FALSE(found.facts.empty());
    const FactLabel& first = found.facts.front();
    EXPECT_EQ(first.job, a.id) << "the article judged";
    EXPECT_EQ(first.fact, "F1");
    EXPECT_TRUE(first.core);
    EXPECT_EQ(first.said, "1:30 to 1:55") << "when the gold says the video says it";
    EXPECT_EQ(first.claim, "The NRC lent them three scales.");
    EXPECT_EQ(first.quote, "The NRC lent us 3 scales");
    ASSERT_FALSE(first.shown.empty());
    EXPECT_THAT(first.shown.front(), HasSubstr("1. ")) << "numbered as the judges saw them";
    EXPECT_EQ(found.facts.size(), lecture_facts().size())
        << "the oracle's article tells every fact: each read";
    EXPECT_THAT(found.facts, Each(Field(&FactLabel::verdict, std::nullopt))) << "no verdict yet: made blind";

    ASSERT_FALSE(found.sentences.empty());
    EXPECT_EQ(found.sentences.front().number, 1U);
    EXPECT_THAT(found.sentences.front().evidence, Not(IsEmpty()));
    EXPECT_THAT(found.sentences.front().evidence.front(), HasSubstr("S"))
        << "the video's lines, as judges see them";

    ASSERT_EQ(found.parts.size(), 3U) << "one outline: a part a section";
    for (const PartLabel& part : found.parts) {
        EXPECT_EQ(part.b_first, b_first("r5", "lecture", 0, part.part)) << "drawn under the file's name";
        EXPECT_FALSE(part.verdict.has_value());
    }
    EXPECT_THAT(found.parts.front().stretch, HasSubstr(" to ")) << "the stretch, by its times";

    // B's shorter question is shown first or second as its part's order
    // says: under twenty names, each order comes up.
    std::set<bool> orders;
    for (int name = 0; name < 20; ++name) {
        const LabelPool drawn = unwrap(pool(entries, pairs, std::format("n{}", name)));
        const auto asked = std::ranges::find_if(drawn.parts, [](const PartLabel& part) {
            return part.first.find("Why?") != std::string::npos ||
                   part.second.find("Why?") != std::string::npos;
        });
        ASSERT_NE(asked, drawn.parts.end()) << "B's shorter question is in a part";
        const std::string& b_version = asked->b_first ? asked->first : asked->second;
        const std::string& a_version = asked->b_first ? asked->second : asked->first;
        EXPECT_THAT(b_version, HasSubstr("Why?")) << "each version where its order puts it";
        EXPECT_THAT(a_version, HasSubstr("Why does it glow?"));
        orders.insert(asked->b_first);
    }
    EXPECT_EQ(orders.size(), 2U) << "both orders drawn";
}

TEST_F(Calibrating, WhatThePanelHasNotReadIsLeftOutAndSaid) {
    const library::Job a = done_article();
    judgement_begun(a);
    const std::vector<RunEntry> entries{entry_of(a, 2)};
    const std::vector<VideoPairs> pairs{
        VideoPairs{.video = "lecture", .pairs = {{entry_of(a), entry_of(a)}}}};
    const LabelPool found = unwrap(pool(entries, pairs));
    EXPECT_THAT(found.facts, IsEmpty());
    EXPECT_THAT(found.parts, IsEmpty());
    ASSERT_EQ(found.notes.size(), 2U);
    EXPECT_EQ(found.notes[0], "lecture replicate 2: not judged by eval-v9 yet: rreport eval judge");
    EXPECT_THAT(found.notes[1], HasSubstr("not read side by side yet: rreport eval compare --flow"));
}

TEST_F(Calibrating, DrawLabelsDrawsEachKindUnderItsOwnSeed) {
    LabelPool pool;
    for (int index = 0; index < 30; ++index) {
        pool.facts.push_back(FactLabel{.fact = std::format("F{}", index)});
        pool.sentences.push_back(SentenceLabel{.number = static_cast<std::size_t>(index + 1)});
    }
    pool.parts.push_back(PartLabel{.part = 0});
    const Labels drawn =
        draw_labels("r5", "eval-v9", "base", "cand", pool, LabelCounts{.facts = 4, .sentences = 5});
    EXPECT_EQ(drawn.name, "r5");
    EXPECT_EQ(drawn.run, "base");
    EXPECT_EQ(drawn.against, "cand");
    EXPECT_EQ(drawn.facts.size(), 4U);
    EXPECT_EQ(drawn.sentences.size(), 5U);
    EXPECT_EQ(drawn.parts.size(), 1U) << "fewer than asked: every one";
    EXPECT_EQ(drawn,
              draw_labels("r5", "eval-v9", "base", "cand", pool, LabelCounts{.facts = 4, .sentences = 5}));
    std::vector<std::string> facts;
    facts.reserve(drawn.facts.size());
    for (const FactLabel& label : drawn.facts) {
        facts.push_back(label.fact);
    }
    std::vector<std::size_t> numbers;
    numbers.reserve(drawn.sentences.size());
    for (const SentenceLabel& label : drawn.sentences) {
        numbers.push_back(label.number);
    }
    const std::vector<std::size_t> facts_drawn = draw(30, 4, "r5/facts");
    const std::vector<std::size_t> sentences_drawn = draw(30, 5, "r5/sentences");
    for (std::size_t index = 0; index < 4; ++index) {
        EXPECT_EQ(facts[index], std::format("F{}", facts_drawn[index]));
    }
    for (std::size_t index = 0; index < 5; ++index) {
        EXPECT_EQ(numbers[index], sentences_drawn[index] + 1) << "each kind its own draw";
    }
}

TEST_F(Calibrating, EachJudgesReadingsAreSetAgainstThePersonsVerdicts) {
    fixtures::Rig rig{{}, fixtures::engines_serving()};
    const library::Job a = done_article();
    const library::Job b = done_article(shorter_question());
    judge(rig, a);
    pair(rig, a, b);
    const std::vector<RunEntry> entries{entry_of(a)};
    const std::vector<VideoPairs> pairs{
        VideoPairs{.video = "lecture", .pairs = {{entry_of(a), entry_of(b)}}}};
    const LabelPool found = unwrap(pool(entries, pairs));
    Labels labels = draw_labels("r5", "eval-v9", "base", "cand", found, LabelCounts{100, 100, 100});
    labels.labeller = "Ada";
    // The person reads as the oracle: every fact told, every sentence
    // supported, B's shorter question better and the rest alike. One fact is
    // skipped, and one is a fact the panel never judged.
    for (FactLabel& label : labels.facts) {
        label.verdict = FactVerdict::told;
    }
    labels.facts.front().verdict.reset();
    labels.facts.push_back(
        FactLabel{.job = "01NOJOB", .video = "lecture", .fact = "F9", .verdict = FactVerdict::told});
    for (SentenceLabel& label : labels.sentences) {
        label.verdict = pipeline::Issue::none;
    }
    for (PartLabel& label : labels.parts) {
        const bool asked =
            label.first.find("Why?") != std::string::npos || label.second.find("Why?") != std::string::npos;
        if (!asked) {
            label.verdict = reason::Preference::tie;
            continue;
        }
        // B's shorter question reads better, wherever it is shown.
        label.verdict = label.b_first ? reason::Preference::first : reason::Preference::second;
    }
    const Calibration calibration = unwrap(calibrate(database(), clock(), labels, panel(), pairs));
    ASSERT_EQ(calibration.kinds.size(), 3U);

    const KindCalibration& facts = calibration.kinds[0];
    EXPECT_EQ(facts.kind, "facts");
    EXPECT_EQ(facts.skipped, 1U);
    EXPECT_EQ(facts.labelled, labels.facts.size() - 1);
    ASSERT_EQ(facts.judges.size(), 2U);
    EXPECT_EQ(facts.judges[0].judge, "commander");
    EXPECT_EQ(facts.judges[0].unread, 1U) << "the fact never judged";
    EXPECT_EQ(facts.judges[0].items, facts.labelled - 1);
    EXPECT_EQ(facts.judges[0].kappa, std::nullopt) << "every verdict told: one class, κ says nothing";
    EXPECT_FALSE(facts.passes());
    EXPECT_THAT(calibration.notes, ::testing::Contains(HasSubstr("fact F9 of lecture replicate 0")));

    const KindCalibration& parts = calibration.kinds[2];
    EXPECT_EQ(parts.labelled, 3U);
    for (const JudgeAgreement& judge : parts.judges) {
        EXPECT_EQ(judge.items, 3U);
        EXPECT_THAT(*judge.kappa, ::testing::DoubleEq(1.0))
            << judge.judge << ": B's part, two ties: all agree";
        EXPECT_FALSE(judge.passes()) << "three items: intervals too wide to clear a bar";
    }

    ASSERT_EQ(calibration.biases.size(), 2U);
    for (const JudgeBiases& judge : calibration.biases) {
        EXPECT_EQ(judge.biases.read, 3U) << judge.judge << ": every part, both orders";
        EXPECT_EQ(judge.biases.flipped, 0U) << "the oracle never prefers what comes first";
        EXPECT_EQ(judge.biases.longer, 0U) << "it preferred the shorter question";
    }
    EXPECT_FALSE(calibration.passes());

    const std::string text = to_text(calibration, labels, panel());
    EXPECT_THAT(text, HasSubstr("labels r5, by Ada: run base against cand, panel eval-v9\n"));
    EXPECT_THAT(text, HasSubstr(std::format("\nfacts: {} labelled, 1 skipped\n", facts.labelled)));
    EXPECT_THAT(text, HasSubstr("the panel eval-v9 does not pass yet:\n"));
    EXPECT_THAT(text, HasSubstr("facts, commander: one class only, where κ says nothing"));
    EXPECT_THAT(text, HasSubstr("parts, infantry: b below a random judge's bar"));
}

// Why a panel does not pass is said by the rules that decide it: a κ of
// exactly 0.4, which floating point computes a hair under it, is at the bar
// (JudgeAgreement::kappa_clears()), and only the class that fails is named.
TEST(CalibrationText, SaysWhyAPanelDoesNotPassByTheRulesThatDecide) {
    constexpr std::array<std::string_view, 2> kTwo{"yes", "no"};
    std::vector<Verdicts> verdicts;  // rows the person's, columns the judge's: {{20, 5}, {10, 15}}
    for (const auto& [person, judge, count] :
         std::array<std::array<std::size_t, 3>, 4>{{{0, 0, 20}, {0, 1, 5}, {1, 0, 10}, {1, 1, 15}}}) {
        verdicts.insert(verdicts.end(), count, Verdicts{.person = person, .judge = judge});
    }
    const JudgeAgreement at_bar = agree("infantry", kTwo, verdicts, 0);
    ASSERT_LT(*at_bar.kappa, kKappaBar) << "(0.7 - 0.5) / (1 - 0.5) in floating point";
    ASSERT_TRUE(at_bar.kappa_clears());
    ASSERT_FALSE(at_bar.classes[0].clears()) << "yes: precision 20 of 30, its lower bound 0.488 under 0.5";
    ASSERT_TRUE(at_bar.classes[1].clears());

    const Calibration calibration{
        .kinds = {KindCalibration{.kind = std::string{kFactsKind}, .labelled = 50, .judges = {at_bar}}}};
    const Labels labels{.name = "r5", .panel = "eval-v9", .run = "base", .labeller = "Ada"};
    const std::string text = to_text(calibration, labels, panel());
    EXPECT_THAT(text, HasSubstr("the panel eval-v9 does not pass yet:\n"
                                "  facts, infantry: yes below a random judge's bar\n"
                                "an interval too wide"))
        << "the class alone: κ is at its bar";
}

// Only what a judge read can be labelled: a fact no judge read, a sentence
// neither did; one judge's reading is enough.
TEST_F(Calibrating, AnItemNoJudgeReadIsNeverDrawn) {
    const library::Job a = done_article();
    const auto unread = [](std::string_view judge) {
        return FactReading{.judge = std::string{judge}, .unread = "too long for its engine"};
    };
    const auto unread_sentence = [](std::string_view judge) {
        return SentenceReading{.judge = std::string{judge}, .unread = "too long for its engine"};
    };
    const Judgement judgement{
        .panel = "eval-v9",
        .judges = {"commander", "infantry"},
        .facts = {JudgedFact{.fact = "F1",
                             .core = true,
                             .candidates = {1},
                             .readings = {unread("commander"), unread("infantry")}},
                  JudgedFact{.fact = "F2",
                             .candidates = {1},
                             .readings = {FactReading{.judge = "commander", .verdict = FactVerdict::told},
                                          unread("infantry")}}},
        .sentences = {
            JudgedSentence{.number = 1,
                           .paragraph = 1,
                           .text = "One.",
                           .readings = {unread_sentence("commander"), unread_sentence("infantry")}},
            JudgedSentence{.number = 2,
                           .paragraph = 1,
                           .text = "Two.",
                           .readings = {SentenceReading{.judge = "commander", .issue = pipeline::Issue::none},
                                        unread_sentence("infantry")}}}};
    done_judgement(a, judgement);
    const std::vector<RunEntry> entries{entry_of(a)};
    const LabelPool found = unwrap(pool(entries, {}));
    ASSERT_EQ(found.facts.size(), 1U) << "F1 read by neither judge";
    EXPECT_EQ(found.facts.front().fact, "F2") << "read by one: enough";
    ASSERT_EQ(found.sentences.size(), 1U);
    EXPECT_EQ(found.sentences.front().number, 2U);
}

// Each judge's biases, over readings written to show them: the commander
// prefers whatever comes first, the infantry the longer version, in both
// orders.
TEST_F(Calibrating, BiasesAreEachJudgesFlipsAndItsPreferencesForTheLongerVersion) {
    const library::Job a = done_article();
    std::string longer = pipeline::testing::oracle_article();
    const std::string asked = "Why does it glow?";
    // Its question padded with words that tell nothing new: the version a
    // fifth longer or more.
    std::string padding;
    for (int round = 0; round < 12; ++round) {
        padding += " and so on, and on, the question asked again";
    }
    longer.replace(longer.find(asked), asked.size(), "Why does it glow," + padding + "?");
    const library::Job b = done_article(longer);
    const ArticlesToCompare articles = unwrap(articles_to_compare(a, b, store()));
    const std::vector<ArticleSection> sections = article_sections(articles.a, articles.a_ledger);
    const auto glow = std::ranges::find_if(sections, [](const ArticleSection& section) {
        return section.text.find("glow") != std::string::npos;
    });
    ASSERT_NE(glow, sections.end());
    const auto index = static_cast<std::size_t>(glow - sections.begin());
    ASSERT_TRUE(glow->first.has_value() && glow->last.has_value());
    const Part part{.first = *glow->first, .last = *glow->last, .a = {index}, .b = {index}};
    const auto read = [](std::string_view judge, reason::Preference a_first, reason::Preference b_first) {
        return PartReading{.judge = std::string{judge}, .a_first = a_first, .b_first = b_first};
    };
    using enum reason::Preference;
    const Pairing pairing{
        .panel = "eval-v9",
        .judges = {"commander", "infantry"},
        .parts = {
            PairedPart{.part = part,
                       .readings = {read("commander", first, first), read("infantry", second, first)}},
            // A tie in both orders: no preference, whatever the lengths.
            PairedPart{.part = part, .readings = {read("commander", tie, tie), read("infantry", tie, tie)}}}};
    done_pairing(a, b, pairing);
    const std::vector<VideoPairs> pairs{
        VideoPairs{.video = "lecture", .pairs = {{entry_of(a), entry_of(b)}}}};
    const Calibration calibration =
        unwrap(calibrate(database(), clock(), Labels{.name = "r5"}, panel(), pairs));
    ASSERT_EQ(calibration.biases.size(), 2U);
    const Biases& commander = calibration.biases[0].biases;
    EXPECT_EQ(commander.read, 2U);
    EXPECT_EQ(commander.flipped, 1U) << "A first, then B first: whatever came first";
    EXPECT_EQ(commander.unequal, 0U) << "a flip prefers neither version";
    const Biases& infantry = calibration.biases[1].biases;
    EXPECT_EQ(infantry.flipped, 0U);
    EXPECT_EQ(infantry.unequal, 1U) << "B's version is a fifth longer or more; a tie prefers neither";
    EXPECT_EQ(infantry.longer, 1U) << "and both orders preferred it";
}

}  // namespace
}  // namespace rr::eval

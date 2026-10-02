// An article read by the eval's frozen judges, on the stages' synthetic
// lecture (tests/unit/pipeline/lecture.hpp): the sentences a judge is shown
// and the video's words beside them; evidence that must be what was shown;
// every item read by every judge of the panel, an answer quoting what it was
// not shown sent back, an item no answer settled left unread; a paragraph
// cut until its judge's engine holds it; scores as each item's mean over its
// judges; and the key, which holds everything the readings rest on.
#include <rr/eval/judgement.hpp>

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/core/clock.hpp>
#include <rr/core/error.hpp>
#include <rr/db/database.hpp>
#include <rr/library/jobs.hpp>
#include <rr/library/library.hpp>
#include <rr/library/stage_store.hpp>
#include <rr/pipeline/atomize_stage.hpp>
#include <rr/pipeline/seams_stage.hpp>
#include <rr/pipeline/source.hpp>
#include <rr/pipeline/transcript_stage.hpp>
#include <rr/prompt/library.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

#include "../pipeline/lecture.hpp"
#include "fixtures.hpp"

namespace rr::eval {
namespace {

using namespace std::chrono_literals;
using knowledge::AtomId;
using knowledge::SentenceId;
using ::testing::AllOf;
using ::testing::Contains;
using ::testing::DoubleEq;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using testkit::unwrap;

const prompt::Library& prompts() {
    static const prompt::Library library = unwrap(prompt::Library::embedded());
    return library;
}

Millis said_from(int sentence) {
    return Millis{(sentence - 1) * 30'000};
}
Millis said_until(int sentence) {
    return said_from(sentence) + 25s;
}

// Three atoms of the lecture: S4, S7 with S8, and S10.
knowledge::AtomLedger lecture_atoms() {
    return knowledge::AtomLedger{
        .atoms = {
            knowledge::Atom{
                .id = AtomId{1}, .text = "The NRC lent us 3 scales for it.", .sentences = {SentenceId{4}}},
            knowledge::Atom{.id = AtomId{2},
                            .text = "Then we added a second pan, and the weight it held doubled.",
                            .sentences = {SentenceId{7}, SentenceId{8}}},
            knowledge::Atom{.id = AtomId{3}, .text = "Why does it glow?", .sentences = {SentenceId{10}}}}};
}

// An article of them: three paragraphs under two sections, a sentence that
// cites nothing, and code, which is no prose.
constexpr std::string_view kArticle =
    "# Weighing radium\n\n"
    "## The scales\n\n"
    "The NRC lent us 3 scales for it. [A1]\n\n"
    "Then we added a second pan, and the weight it held doubled. [A2] So the scales were checked.\n\n"
    "```\nweigh(sample) [A1]\n```\n\n"
    "## The glow\n\n"
    "Why does it glow? [A3]\n";

// Five facts: three the article tells; one only its BM25 neighbours could,
// and do not; one no sentence of it comes near.
std::vector<Fact> lecture_facts() {
    return {Fact{.id = "F1",
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
                 .claim = "Ada asks why radium glows."},
            Fact{.id = "F4",
                 .start = said_from(29),
                 .end = said_until(29),
                 .importance = Importance::core,
                 .quote = "we need better scales",
                 .claim = "Ada thinks they need better scales."},
            Fact{.id = "F5",
                 .start = said_from(6),
                 .end = said_until(6),
                 .importance = Importance::detail,
                 .quote = "Its half-life is long",
                 .claim = "Its half-life is long."}};
}

ArticleToJudge lecture_article() {
    return ArticleToJudge{.title = "Weighing radium",
                          .channel = "Science Hour",
                          .transcript = pipeline::testing::lecture(),
                          .ledger = lecture_atoms(),
                          .article = knowledge::Article{.markdown = std::string{kArticle}},
                          .facts = lecture_facts()};
}

// A panel of the fixtures' two engines, pinning this rreport's prompts.
Panel panel() {
    Panel made{.name = "eval-v9", .procedure = kJudgingProcedure};
    for (const std::string_view name : kJudgePrompts) {
        const prompt::Version& current = prompts().find(name)->current();
        made.prompts.push_back(
            PanelPrompt{.name = std::string{name}, .version = current.number, .hash = current.hash});
    }
    made.judges = {
        PanelJudge{
            .role = "commander", .model = "qwen-commander", .sha256 = std::string{fixtures::kQwenFile}},
        PanelJudge{
            .role = "infantry", .model = "gemma-infantry", .sha256 = std::string{fixtures::kGemmaFile}}};
    return made;
}

const library::Job kJob{
    .id = "01JUDGE", .source = "01ARTICLE", .tier = "eval-v9", .kind = library::JobKind::judgement};

// `article` judged on `rig`'s army, as the executor runs a stage.
Result<pipeline::StageOutput> judge(fixtures::Rig& rig, ArticleToJudge article = lecture_article(),
                                    Panel by = panel()) {
    const JudgementStage stage{std::move(by), std::move(article)};
    pipeline::StageContext context{.job = &kJob, .army = rig.raise(), .prompts = &prompts()};
    RR_TRY_ASSIGN(context.fingerprint, stage.fingerprint(context));
    return stage.run(context);
}

TEST(ArticleSentences, AreItsProseSentencesWithoutMarkersEachWithItsParagraphAndSection) {
    const std::vector<ArticleSentence> sentences = article_sentences(
        knowledge::Article{.markdown = std::string{kArticle} + "\nThe A9 atom [A9].\n"}, lecture_atoms());
    ASSERT_EQ(sentences.size(), 5U) << "headings and code are no sentences";
    EXPECT_EQ(sentences[0], (ArticleSentence{.number = 1,
                                             .paragraph = 1,
                                             .section = "The scales",
                                             .text = "The NRC lent us 3 scales for it.",
                                             .cited = {AtomId{1}}}));
    EXPECT_EQ(sentences[2], (ArticleSentence{.number = 3,
                                             .paragraph = 2,
                                             .section = "The scales",
                                             .text = "So the scales were checked.",
                                             .cited = {}}));
    EXPECT_EQ(sentences[3].section, "The glow");
    EXPECT_EQ(sentences[3].paragraph, 3U);
    EXPECT_THAT(sentences[4].cited, IsEmpty()) << "A9 is no atom of the ledger";
}

TEST(FactCandidates, AreTheSentencesCitingItsSentencesThenTheBestByBm25InTheArticlesOrder) {
    const knowledge::Transcript transcript = pipeline::testing::lecture();
    const std::vector<ArticleSentence> sentences =
        article_sentences(knowledge::Article{.markdown = std::string{kArticle}}, lecture_atoms());
    const std::vector<Fact> facts = lecture_facts();
    EXPECT_THAT(fact_candidates(facts[1], sentences, transcript, lecture_atoms()), Contains(2U))
        << "the sentence citing A2, which S7 and S8 say";
    const std::vector<std::size_t> scales = fact_candidates(facts[3], sentences, transcript, lecture_atoms());
    EXPECT_THAT(scales, AllOf(Contains(1U), Contains(3U)))
        << "no atom says S29: the sentences about scales, by BM25";
    EXPECT_TRUE(std::ranges::is_sorted(scales)) << "in the article's order";
    EXPECT_THAT(fact_candidates(facts[4], sentences, transcript, lecture_atoms()), IsEmpty())
        << "nothing of the article comes near the half-life";
}

TEST(FactCandidates, ASentenceCitingTheFactsAtomsIsShownInWhateverWordsItTellsIt) {
    knowledge::AtomLedger ledger = lecture_atoms();
    ledger.atoms.push_back(
        knowledge::Atom{.id = AtomId{4}, .text = "Curie called it precious.", .sentences = {SentenceId{9}}});
    const knowledge::Article article{
        .markdown = std::string{kArticle} + "\nA famous chemist once praised the element highly. [A4]\n"};
    const Fact curie{.id = "F6",
                     .start = said_from(9),
                     .end = said_until(9),
                     .importance = Importance::core,
                     .quote = "Marie Curie called radium",
                     .claim = "Marie Curie called radium precious."};
    EXPECT_THAT(
        fact_candidates(curie, article_sentences(article, ledger), pipeline::testing::lecture(), ledger),
        ElementsAre(5U))
        << "no word in common: only its citation finds it";
}

TEST(FactCandidates, AreInTheArticlesOrderWhereverTheyWereFound) {
    const std::vector<ArticleSentence> sentences =
        article_sentences(knowledge::Article{.markdown = std::string{kArticle}}, lecture_atoms());
    const Fact glow{.id = "F7",
                    .start = said_from(10),
                    .end = said_until(10),
                    .importance = Importance::core,
                    .quote = "Why does it glow",
                    .claim = "Ada asks why scales glow."};
    const std::vector<std::size_t> found =
        fact_candidates(glow, sentences, pipeline::testing::lecture(), lecture_atoms());
    ASSERT_GE(found.size(), 2U) << "sentence 4 cites it, and BM25 adds those about scales";
    EXPECT_TRUE(std::ranges::is_sorted(found));
    EXPECT_EQ(found.back(), 4U);
}

TEST(FactCandidates, AreTwelveAtMost) {
    std::string markdown = "## The scales\n\n";
    for (int told = 0; told < 15; ++told) {
        markdown += "The NRC lent us 3 scales for it. [A1]\n\n";
    }
    const std::vector<ArticleSentence> sentences =
        article_sentences(knowledge::Article{.markdown = markdown}, lecture_atoms());
    const std::vector<std::size_t> found =
        fact_candidates(lecture_facts()[0], sentences, pipeline::testing::lecture(), lecture_atoms());
    EXPECT_EQ(found.size(), kMostCandidates);
    EXPECT_EQ(found.front(), 1U);
    EXPECT_EQ(found.back(), 12U) << "the first twelve citing it";
}

TEST(EvidenceOf, IsTheVideosSentencesOfTheAtomsCitedOrTheParagraphsWhenNone) {
    const std::vector<ArticleSentence> sentences =
        article_sentences(knowledge::Article{.markdown = std::string{kArticle}}, lecture_atoms());
    const std::vector<ArticleSentence> second{sentences[1], sentences[2]};
    EXPECT_THAT(evidence_of(sentences[0], {sentences[0]}, lecture_atoms()), ElementsAre(SentenceId{4}));
    EXPECT_THAT(evidence_of(sentences[1], second, lecture_atoms()),
                ElementsAre(SentenceId{7}, SentenceId{8}));
    EXPECT_THAT(evidence_of(sentences[2], second, lecture_atoms()), ElementsAre(SentenceId{7}, SentenceId{8}))
        << "it cites nothing: its paragraph's";
    const ArticleSentence again{.number = 9, .paragraph = 2, .text = "It doubled.", .cited = {AtomId{2}}};
    EXPECT_THAT(evidence_of(sentences[2], {sentences[1], again, sentences[2]}, lecture_atoms()),
                ElementsAre(SentenceId{7}, SentenceId{8}))
        << "each sentence of the video once, though two sentences cite it";
}

TEST(EvidenceProblems, AQuoteMustBeWhatWasShownWordForWord) {
    const std::vector<std::string> shown{"The NRC lent us 3 scales for it.", "So the scales were checked."};
    EXPECT_THAT(evidence_problems("1: \"The NRC lent us 3 scales\"", shown, true), IsEmpty());
    EXPECT_THAT(evidence_problems("2: \u201Cso, the scales WERE checked\u201D", shown, true), IsEmpty())
        << "curly quotes; case and punctuation aside";
    EXPECT_THAT(evidence_problems("", shown, false), IsEmpty()) << "nothing quoted, nothing to check";
    EXPECT_THAT(evidence_problems("It is in sentence 1.", shown, true),
                ElementsAre(HasSubstr("the evidence quotes none")));
    EXPECT_THAT(evidence_problems("1: \"The NRC lent us three scales\"", shown, false),
                ElementsAre(HasSubstr("\"The NRC lent us three scales\" is not in what you were shown")));
}

// A judgement of four facts and four sentences, each read as its test says.
Judgement four_items() {
    const auto fact = [](std::string id, bool core, const std::vector<std::optional<FactVerdict>>& verdicts) {
        JudgedFact judged{.fact = std::move(id), .core = core, .candidates = {1}};
        for (const std::optional<FactVerdict>& verdict : verdicts) {
            judged.readings.push_back(
                FactReading{.judge = "j", .verdict = verdict, .unread = verdict ? "" : "no answer"});
        }
        if (verdicts.empty()) {
            judged.candidates.clear();
        }
        return judged;
    };
    const auto sentence = [](std::size_t number, const std::vector<std::optional<pipeline::Issue>>& issues) {
        JudgedSentence judged{.number = number, .paragraph = 1, .text = "A sentence."};
        for (const std::optional<pipeline::Issue>& issue : issues) {
            judged.readings.push_back(
                SentenceReading{.judge = "j", .issue = issue, .unread = issue ? "" : "no answer"});
        }
        return judged;
    };
    return Judgement{.panel = "eval-v9",
                     .judges = {"commander", "infantry"},
                     .facts = {fact("F1", true, {FactVerdict::told, FactVerdict::partly}),
                               fact("F2", false, {FactVerdict::told, FactVerdict::told}),
                               fact("F3", true, {}),  // no sentence near it: not told, no judge asked
                               fact("F4", true, {std::nullopt, std::nullopt})},  // unread by both: left out
                     .sentences = {sentence(1, {pipeline::Issue::none, pipeline::Issue::none}),
                                   sentence(2, {pipeline::Issue::adds, pipeline::Issue::none}),
                                   sentence(3, {std::nullopt, pipeline::Issue::contradicts}),
                                   sentence(4, {std::nullopt, std::nullopt})}};  // unread by both: left out
}

double score(const std::vector<Score>& scores, std::string_view metric) {
    const auto found = std::ranges::find(scores, metric, &Score::metric);
    return found == scores.end() ? -1.0 : found->value;
}

TEST(JudgedScores, AreEachItemsMeanOverTheJudgesThatReadIt) {
    const std::vector<Score> scores = judged_scores(four_items());
    EXPECT_THAT(score(scores, "facts_told"), DoubleEq((0.5 + 1.0 + 0.0) / 3)) << "F4 read by none: left out";
    EXPECT_THAT(score(scores, "facts_told_partly"), DoubleEq((1.0 + 1.0 + 0.0) / 3));
    EXPECT_THAT(score(scores, "core_facts_told"), DoubleEq((0.5 + 0.0) / 2)) << "F1 and F3: F2 is a detail";
    EXPECT_THAT(score(scores, "facts_agreement"), DoubleEq(0.5))
        << "F2 alike, F1 not; F3 and F4 read by fewer than two";
    EXPECT_THAT(score(scores, "sentences_supported"), DoubleEq((1.0 + 0.5 + 0.0) / 3))
        << "S3: the one reading there is; S4 read by none: left out";
    EXPECT_THAT(score(scores, "adds"), DoubleEq(0.5 / 3));
    EXPECT_THAT(score(scores, "contradicts"), DoubleEq(1.0 / 3));
    EXPECT_THAT(score(scores, "softens"), DoubleEq(0.0));
    EXPECT_THAT(score(scores, "sentences_agreement"), DoubleEq(0.5))
        << "S1 alike, S2 not; S3 read by one judge only";
    std::vector<std::string> metrics;
    metrics.reserve(scores.size());
    for (const Score& kept : scores) {
        metrics.push_back(kept.metric);
    }
    EXPECT_THAT(metrics, ElementsAre("facts_told", "facts_told_partly", "core_facts_told", "facts_agreement",
                                     "sentences_supported", "adds", "softens", "intensifies", "misattributes",
                                     "contradicts", "sentences_agreement"));
    EXPECT_THAT(judged_scores(Judgement{.panel = "eval-v9", .judges = {"commander"}}), IsEmpty())
        << "a share of nothing is no score";
}

TEST(JudgedScores, CountEachIssueUnderItsOwnName) {
    for (const pipeline::Issue issue :
         {pipeline::Issue::none, pipeline::Issue::adds, pipeline::Issue::softens,
          pipeline::Issue::intensifies, pipeline::Issue::misattributes, pipeline::Issue::contradicts}) {
        const Judgement judgement{.panel = "eval-v9",
                                  .judges = {"commander"},
                                  .sentences = {JudgedSentence{
                                      .number = 1,
                                      .paragraph = 1,
                                      .text = "A sentence.",
                                      .readings = {SentenceReading{.judge = "commander", .issue = issue}}}}};
        const std::string_view named = issue == pipeline::Issue::none
                                           ? std::string_view{"sentences_supported"}
                                           : pipeline::to_string(issue);
        const std::vector<Score> scores = judged_scores(judgement);
        ASSERT_EQ(scores.size(), 6U) << "read by one judge: no agreement to score";
        for (const Score& kept : scores) {
            EXPECT_EQ(kept.value, kept.metric == named ? 1.0 : 0.0)
                << kept.metric << ", of a sentence read as " << pipeline::to_string(issue);
        }
    }
}

TEST(Judgement, RoundTripsThroughItsArtifact) {
    Judgement judgement = four_items();
    judgement.sentences[1].evidence = {SentenceId{7}, SentenceId{8}};
    judgement.sentences[1].readings[0].reading = "It adds a cause.";
    EXPECT_EQ(unwrap(judgement_from_json(to_json(judgement))), judgement);
    json::Json broken = to_json(judgement);
    broken["facts"][0]["readings"][0].erase("verdict");
    EXPECT_ERRC(judgement_from_json(broken), Errc::invalid_argument) << "a reading with no verdict says why";
    EXPECT_ERRC(judgement_from_json(json::Json{{"panel", "eval-v9"}}), Errc::invalid_argument);
}

TEST(JudgementStage, ReadsEveryFactAndSentenceByEveryJudgeOfThePanel) {
    fixtures::Rig rig{{}, fixtures::engines_serving()};
    const pipeline::StageOutput output = unwrap(judge(rig));
    EXPECT_THAT(output.findings, IsEmpty());
    const Judgement judgement = unwrap(judgement_from_json(output.artifact));
    EXPECT_EQ(judgement.judges, (std::vector<std::string>{"commander", "infantry"}));

    std::vector<std::string> told;
    for (const JudgedFact& fact : judgement.facts) {
        std::string verdicts;
        for (const FactReading& reading : fact.readings) {
            verdicts += std::format("{}{}", verdicts.empty() ? "" : " ", to_string(reading.verdict.value()));
        }
        told.push_back(std::format("{}: {}", fact.fact, verdicts.empty() ? "no judge asked" : verdicts));
    }
    EXPECT_THAT(told, ElementsAre("F1: told told", "F2: told told", "F3: told told", "F4: not_told not_told",
                                  "F5: no judge asked"));

    ASSERT_EQ(judgement.sentences.size(), 4U);
    EXPECT_THAT(judgement.sentences[2].evidence, ElementsAre(SentenceId{7}, SentenceId{8}));
    for (const JudgedSentence& sentence : judgement.sentences) {
        ASSERT_EQ(sentence.readings.size(), 2U);
        EXPECT_EQ(sentence.readings[0].issue, pipeline::Issue::none);
        EXPECT_EQ(sentence.readings[1].issue, pipeline::Issue::none);
    }

    const std::vector<llm::ChatRequest> facts = rig.requests("eval_fact");
    const std::vector<llm::ChatRequest> paragraphs = rig.requests("eval_support");
    EXPECT_EQ(facts.size(), 8U) << "four facts with sentences near them, by two judges";
    EXPECT_EQ(paragraphs.size(), 6U) << "three paragraphs, by two judges";
    for (const llm::ChatRequest& request : facts) {
        EXPECT_EQ(request.temperature, 0.0) << "greedy";
        EXPECT_EQ(request.thinking, false);
        EXPECT_THAT(request.messages.front().text(),
                    HasSubstr("The video is \"Weighing radium\", by Science Hour."));
    }
    const auto second_paragraph = std::ranges::find_if(paragraphs, [](const llm::ChatRequest& request) {
        return request.messages.back().text().find("paragraph 2, in \"The scales\"") != std::string::npos;
    });
    ASSERT_NE(second_paragraph, paragraphs.end());
    EXPECT_THAT(
        second_paragraph->messages.back().text(),
        HasSubstr(
            "2. So the scales were checked.\n   S7 [3:00] Then we added a second pan, and the weight it "
            "held\n   S8 [3:30] doubled."))
        << "the video's own words, not the atoms'";

    const std::vector<Score> scores = judged_scores(judgement);
    EXPECT_THAT(score(scores, "facts_told"), DoubleEq(3.0 / 5));
    EXPECT_THAT(score(scores, "core_facts_told"), DoubleEq(2.0 / 3));
    EXPECT_THAT(score(scores, "sentences_supported"), DoubleEq(1.0));
}

TEST(JudgementStage, AnAnswerQuotingWhatItWasNotShownIsSentBackOnceThenTheItemIsUnread) {
    const auto made_up = [](const std::string& /*answer*/) {
        return std::string{R"({"evidence": "1: \"the scales were made up\"", "verdict": "told"})"};
    };
    fixtures::Rig once{{oracle::edit("made-up evidence",
                                     oracle::When{.kind = "eval_fact", .model = "gemma-infantry"}, made_up)},
                       fixtures::engines_serving()};
    const Judgement repaired = unwrap(judgement_from_json(unwrap(judge(once)).artifact));
    EXPECT_EQ(repaired.facts[0].readings[1].verdict, FactVerdict::told)
        << "the second answer quoted what it was shown";

    fixtures::Rig always{
        {oracle::edit("made-up evidence",
                      oracle::When{.kind = "eval_fact", .model = "gemma-infantry", .calls = 0}, made_up)},
        fixtures::engines_serving()};
    const pipeline::StageOutput output = unwrap(judge(always));
    const Judgement unread = unwrap(judgement_from_json(output.artifact));
    EXPECT_EQ(unread.facts[0].readings[0].verdict, FactVerdict::told) << "the commander read it";
    EXPECT_EQ(unread.facts[0].readings[1].verdict, std::nullopt);
    EXPECT_THAT(unread.facts[0].readings[1].unread,
                HasSubstr("\"the scales were made up\" is not in what you were shown"));
    EXPECT_THAT(output.findings, Contains(HasSubstr("F1: the infantry could not read it")));
    for (const llm::ChatRequest& request : always.requests("eval_fact")) {
        EXPECT_EQ(request.thinking, false) << "a frozen judge is asked again as it was, never thinking";
        EXPECT_EQ(request.temperature, 0.0);
    }
    EXPECT_THAT(score(judged_scores(unread), "facts_told"), DoubleEq(3.0 / 5))
        << "the commander's readings alone";
}

TEST(JudgementStage, AParagraphItsJudgesEngineCannotHoldIsCutUntilEachPartFits) {
    // Paragraph 2's atom said over fourteen sentences of the video: each of
    // its sentences is shown them all. As the prompts are now, the infantry
    // holds one of its sentences from 1,700 tokens, and both from 2,300.
    ArticleToJudge wide = lecture_article();
    wide.ledger.atoms[1].sentences.clear();
    for (std::uint32_t id = 7; id <= 20; ++id) {
        wide.ledger.atoms[1].sentences.emplace_back(id);
    }
    const auto infantry = [](int holds) {
        return fixtures::engines_serving(fixtures::kQwenFile, fixtures::kGemmaFile, 8192, holds);
    };

    fixtures::Rig narrow{{}, infantry(1950)};
    const Judgement cut = unwrap(judgement_from_json(unwrap(judge(narrow, wide)).artifact));
    const std::vector<llm::ChatRequest> paragraphs = narrow.requests("eval_support");
    EXPECT_EQ(std::ranges::count(paragraphs, std::string{"gemma-infantry"}, &llm::ChatRequest::model), 4)
        << "paragraph 2 in two parts";
    for (const llm::ChatRequest& request : paragraphs) {
        if (request.model == "gemma-infantry" &&
            request.messages.back().text().find("paragraph 2") != std::string::npos) {
            EXPECT_THAT(request.messages.back().text(), HasSubstr("(its sentence "));
        }
    }
    for (const JudgedSentence& sentence : cut.sentences) {
        EXPECT_EQ(sentence.readings[1].issue, pipeline::Issue::none) << "every sentence read all the same";
    }

    fixtures::Rig tight{{}, infantry(1450)};
    const pipeline::StageOutput tight_output = unwrap(judge(tight, wide));
    const Judgement too_long = unwrap(judgement_from_json(tight_output.artifact));
    EXPECT_THAT(too_long.sentences[1].readings[1].unread,
                HasSubstr("holds no mission reading it, even alone"));
    EXPECT_THAT(too_long.sentences[2].readings[1].unread,
                HasSubstr("holds no mission reading it, even alone"));
    EXPECT_EQ(too_long.sentences[0].readings[1].issue, pipeline::Issue::none) << "the other paragraphs fit";
    EXPECT_EQ(too_long.sentences[3].readings[1].issue, pipeline::Issue::none);
    EXPECT_EQ(tight_output.findings.size(), 2U);

    fixtures::Rig tiny{{}, infantry(600)};
    const Judgement nothing = unwrap(judgement_from_json(unwrap(judge(tiny, wide)).artifact));
    EXPECT_THAT(nothing.facts[0].readings[1].unread,
                HasSubstr("the infantry's engine holds no mission reading it"))
        << "a fact's mission too: unread, never a squad refused whole";
    EXPECT_EQ(nothing.facts[0].readings[0].verdict, FactVerdict::told) << "the commander's engine holds them";
}

TEST(JudgementStage, WithoutThePanelsModelsTheJudgementNeedsYou) {
    const library::Job job = kJob;
    const JudgementStage stage{panel(), lecture_article()};
    EXPECT_ERRC(stage.fingerprint(pipeline::StageContext{.job = &job, .prompts = &prompts()}),
                Errc::needs_user)
        << "no pod";
    fixtures::Rig other{
        {}, fixtures::engines_serving("aab65c67ef0dad127960efef9247f1832bca105faa1c7a052cc039b223cf86a1")};
    EXPECT_ERRC(
        stage.fingerprint(pipeline::StageContext{.job = &job, .army = other.raise(), .prompts = &prompts()}),
        Errc::needs_user)
        << "the 8-bit commander is not the panel's";
    Panel stale = panel();
    stale.prompts[0].hash = "0123";
    fixtures::Rig rig{{}, fixtures::engines_serving()};
    EXPECT_ERRC(
        JudgementStage(stale, lecture_article())
            .fingerprint(pipeline::StageContext{.job = &job, .army = rig.raise(), .prompts = &prompts()}),
        Errc::conflict);
}

TEST(JudgementStage, ItsKeyHoldsWhatTheJudgesReadAndHowTheyAreAsked) {
    const library::Job job = kJob;
    const auto key = [&](ArticleToJudge article,
                         std::vector<agents::Engine> engines = fixtures::engines_serving()) {
        fixtures::Rig rig{{}, std::move(engines)};
        const JudgementStage stage{panel(), std::move(article)};
        return unwrap(stage.fingerprint(
            pipeline::StageContext{.job = &job, .army = rig.raise(), .prompts = &prompts()}));
    };
    const json::Json same = key(lecture_article());
    EXPECT_EQ(key(lecture_article()), same);
    ArticleToJudge edited = lecture_article();
    edited.article.markdown += "\nOne more sentence. [A3]\n";
    EXPECT_NE(key(edited), same);
    ArticleToJudge refacted = lecture_article();
    refacted.facts[0].importance = Importance::detail;
    EXPECT_NE(key(refacted), same) << "the gold, corrected";
    ArticleToJudge renamed = lecture_article();
    renamed.title = "Weighing polonium";
    EXPECT_NE(key(renamed), same) << "the prompts show the title";
    EXPECT_NE(key(lecture_article(),
                  fixtures::engines_serving(fixtures::kQwenFile, fixtures::kGemmaFile, 8192, 2048)),
              same)
        << "how much an engine holds decides how paragraphs are cut";
}

// Judgements on a real library: an article job done on the lecture, its
// artifacts committed as a job done has them.
class Judging : public ::testing::Test {
protected:
    [[nodiscard]] library::Job done_article() {
        library::Job job = unwrap(jobs_.create("talk.vtt", "max", 0));
        const auto commit = [&](std::string_view stage, json::Json artifact) {
            clock_.advance(1s);
            (void)unwrap(
                store_.commit(library::StageResult{.job_id = job.id,
                                                   .stage = std::string{stage},
                                                   .version = 1,
                                                   .key = std::string{stage},  // a key a stage: shared
                                                   .schema = "artifacts/x",
                                                   .artifact = std::move(artifact),
                                                   .started_at = clock_.now()}));
        };
        commit(pipeline::SourceStage::kName, pipeline::to_json(pipeline::testing::source()));
        commit(pipeline::TranscriptStage::kName, knowledge::to_json(pipeline::testing::lecture()));
        commit(pipeline::AtomizeStage::kName, knowledge::to_json(pipeline::testing::lecture_ledger()));
        commit(pipeline::SeamsStage::kName,
               knowledge::to_json(knowledge::Article{.markdown = pipeline::testing::oracle_article()}));
        job.status = library::JobStatus::done;
        job.title = "Weighing radium";
        return unwrap(jobs_.save(job));
    }

    [[nodiscard]] Result<JudgementRun> judge(fixtures::Rig& rig, const library::Job& article) {
        RR_TRY_ASSIGN(ArticleToJudge read, article_to_judge(article, store_, lecture_facts()));
        RR_TRY_ASSIGN(library::Job job, start_judgement(database_, clock_, article, panel()));
        return run_judgement(
            std::move(job), panel(), std::move(read), database_, clock_,
            pipeline::StageContext{
                .army = rig.raise(), .prompts = &prompts(), .work_directory = dir_ / "jobs"});
    }

    [[nodiscard]] db::Database& database() { return database_; }
    [[nodiscard]] FakeClock& clock() { return clock_; }
    [[nodiscard]] library::Jobs& jobs() { return jobs_; }
    [[nodiscard]] const library::StageStore& store() const { return store_; }
    [[nodiscard]] std::filesystem::path work() const { return dir_ / "jobs"; }

private:
    testkit::TempDir dir_;
    db::Database database_ = unwrap(library::open_database(dir_ / "library.db"));
    FakeClock clock_;
    library::Jobs jobs_{database_, clock_};
    library::StageStore store_{database_, clock_};
};

TEST_F(Judging, AnArticleHasOneJudgementAPanelTakenUpAfter) {
    const library::Job article = done_article();
    const library::Job judgement = unwrap(start_judgement(database(), clock(), article, panel()));
    EXPECT_EQ(judgement.kind, library::JobKind::judgement);
    EXPECT_EQ(judgement.source, article.id) << "the job it judges";
    EXPECT_EQ(judgement.tier, "eval-v9") << "the panel that judges it";
    EXPECT_EQ(judgement.salt, 0);
    EXPECT_EQ(judgement.title, "Weighing radium") << "listed by the video it judges";
    EXPECT_EQ(unwrap(start_judgement(database(), clock(), article, panel())).id, judgement.id);
    Panel other = panel();
    other.name = "eval-v10";
    EXPECT_NE(unwrap(start_judgement(database(), clock(), article, other)).id, judgement.id)
        << "each panel its own judgement";
}

TEST_F(Judging, AJudgementDoneIsScoredAndAnArticleUnchangedIsNeverReadTwice) {
    const library::Job article = done_article();
    fixtures::Rig rig{{}, fixtures::engines_serving()};
    const JudgementRun judged = unwrap(judge(rig, article));
    EXPECT_EQ(judged.run.job.status, library::JobStatus::done);
    ASSERT_TRUE(judged.scoring.has_value());
    EXPECT_EQ(judged.scoring->scorer, "judges@eval-v9");
    EXPECT_THAT(score(judged.scoring->scores, "facts_told"), DoubleEq(1.0))
        << "the oracle's article tells every sentence of the lecture, and each fact with it";
    EXPECT_THAT(score(judged.scoring->scores, "sentences_supported"), DoubleEq(1.0));
    const std::size_t asked = rig.log.requests().size();
    ASSERT_GT(asked, 0U);

    EXPECT_EQ(unwrap(judge(rig, article)).scoring, judged.scoring);
    EXPECT_EQ(rig.log.requests().size(), asked) << "its judgement found done";
    EXPECT_EQ(unwrap(judge(rig, done_article())).scoring, judged.scoring);
    EXPECT_EQ(rig.log.requests().size(), asked)
        << "another job's article, the same: its judgement's stage found in the cache";
}

TEST_F(Judging, AJudgementWithoutThePodNeedsYouAndIsNotScored) {
    const library::Job article = done_article();
    library::Job job = unwrap(start_judgement(database(), clock(), article, panel()));
    const JudgementRun waiting = unwrap(run_judgement(
        std::move(job), panel(), unwrap(article_to_judge(article, store(), lecture_facts())), database(),
        clock(), pipeline::StageContext{.prompts = &prompts(), .work_directory = work()}));
    EXPECT_EQ(waiting.run.job.status, library::JobStatus::needs_user);
    EXPECT_EQ(waiting.scoring, std::nullopt);
}

TEST_F(Judging, AJobWithNoArticleHasNothingToJudge) {
    const library::Job started = unwrap(jobs().create("talk.vtt", "max", 0));
    EXPECT_ERRC(article_to_judge(started, store(), {}), Errc::invalid_argument);
}

}  // namespace
}  // namespace rr::eval

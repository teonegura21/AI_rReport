// Two articles of one video read side by side by the eval's frozen judges
// (rr/eval/flow.hpp), on the stages' synthetic lecture: an article's
// sections placed on the video by what they cite, the two articles' sections
// joined where they overlap, every part read by every judge in both orders,
// a preference counted only when it survives the swap, a part an engine
// cannot hold left unread, and pairings as jobs on a real library.
#include <rr/eval/flow.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
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
using reason::Preference;
using ::testing::DoubleEq;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::Optional;
using ::testing::Pair;
using testkit::unwrap;

const prompt::Library& prompts() {
    static const prompt::Library library = unwrap(prompt::Library::embedded());
    return library;
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

// An article of them: a title, then two sections, the first of two
// paragraphs, one of whose sentences cites nothing.
constexpr std::string_view kArticle =
    "# Weighing radium\n\n"
    "## The scales\n\n"
    "The NRC lent us 3 scales for it. [A1]\n\n"
    "Then we added a second pan, and the weight it held doubled. [A2] So the scales were checked.\n\n"
    "## The glow\n\n"
    "Why does it glow? [A3]\n";

// The same, its glow told in fewer words.
constexpr std::string_view kShorterGlow =
    "# Weighing radium\n\n"
    "## The scales\n\n"
    "The NRC lent us 3 scales for it. [A1]\n\n"
    "Then we added a second pan, and the weight it held doubled. [A2] So the scales were checked.\n\n"
    "## The glow\n\n"
    "It glows. [A3]\n";

ArticleSection placed(std::string heading, int first, int last) {
    return ArticleSection{.heading = std::move(heading),
                          .text = "Text.",
                          .first = SentenceId{static_cast<std::uint32_t>(first)},
                          .last = SentenceId{static_cast<std::uint32_t>(last)}};
}

ArticleSection uncited(std::string heading) {
    return ArticleSection{.heading = std::move(heading), .text = "Text."};
}

Part part(int first, int last, std::vector<std::size_t> a, std::vector<std::size_t> b) {
    return Part{.first = SentenceId{static_cast<std::uint32_t>(first)},
                .last = SentenceId{static_cast<std::uint32_t>(last)},
                .a = std::move(a),
                .b = std::move(b)};
}

TEST(ArticleSections, AreItsHeadingsEachWithItsProseWithoutMarkersAndWhereItsAtomsWereSaid) {
    const std::vector<ArticleSection> sections =
        article_sections(knowledge::Article{.markdown = std::string{kArticle}}, lecture_atoms());
    ASSERT_EQ(sections.size(), 3U);
    EXPECT_EQ(sections[0], (ArticleSection{.heading = "Weighing radium", .text = "# Weighing radium"}))
        << "the title cites nothing";
    EXPECT_EQ(sections[1],
              (ArticleSection{.heading = "The scales",
                              .text = "## The scales\n\nThe NRC lent us 3 scales for it.\n\nThen we "
                                      "added a second pan, and the weight it held doubled. So the "
                                      "scales were checked.",
                              .first = SentenceId{4},
                              .last = SentenceId{8}}));
    EXPECT_EQ(sections[2].first, SentenceId{10});
    EXPECT_EQ(sections[2].last, SentenceId{10});
    EXPECT_EQ(sections[2].text, "## The glow\n\nWhy does it glow?");

    const std::vector<ArticleSection> unknown = article_sections(
        knowledge::Article{.markdown = std::string{kArticle} + "\nAn atom no ledger has. [A9]\n"},
        lecture_atoms());
    ASSERT_EQ(unknown.size(), 3U);
    EXPECT_EQ(unknown[2].first, SentenceId{10}) << "a marker of no atom places nothing";
    EXPECT_EQ(unknown[2].last, SentenceId{10});
}

TEST(PartsOf, AreTheSectionsOfOneOutlineSectionBySection) {
    const std::vector<ArticleSection> sections{uncited("Title"), placed("The scales", 4, 8),
                                               placed("The glow", 10, 10)};
    EXPECT_THAT(parts_of(sections, sections), ElementsAre(part(4, 8, {0, 1}, {0, 1}), part(10, 10, {2}, {2})))
        << "the title goes with the section after it";
}

TEST(PartsOf, JoinSectionsCutDifferentlyWhereTheyOverlap) {
    const std::vector<ArticleSection> a{placed("One", 1, 4), placed("Two", 5, 8), placed("Three", 9, 12)};
    const std::vector<ArticleSection> b{placed("First half", 1, 8), placed("Last", 9, 12)};
    EXPECT_THAT(parts_of(a, b), ElementsAre(part(1, 8, {0, 1}, {0}), part(9, 12, {2}, {1})));
    const std::vector<ArticleSection> skewed{placed("One", 1, 5), placed("Two", 6, 12)};
    EXPECT_THAT(parts_of(a, skewed), ElementsAre(part(1, 12, {0, 1, 2}, {0, 1}))) << "a chain of overlaps";
    const std::vector<ArticleSection> told_late_first{placed("Late", 9, 12), placed("Early", 1, 4)};
    EXPECT_THAT(parts_of(told_late_first, std::vector{placed("All", 1, 12)}),
                ElementsAre(part(1, 12, {0, 1}, {0})))
        << "a part's sections in its article's order, not the video's";
}

TEST(PartsOf, GiveASectionCitingNothingToTheOneBeforeAndLeaveOutWhatOneArticleAloneTells) {
    const std::vector<ArticleSection> a{placed("One", 1, 4), uncited("Aside"), placed("Late", 20, 25)};
    const std::vector<ArticleSection> b{placed("One", 1, 4)};
    EXPECT_THAT(parts_of(a, b), ElementsAre(part(1, 4, {0, 1}, {0})))
        << "the aside with One; S20 to S25, A's alone, no part";
    EXPECT_THAT(parts_of({}, b), IsEmpty());
    EXPECT_THAT(parts_of({uncited("Nothing")}, b), IsEmpty()) << "a section placed nowhere";
}

PartReading read(std::string judge, Preference a_first, Preference b_first) {
    return PartReading{.judge = std::move(judge), .a_first = a_first, .b_first = b_first, .reasons = "Why."};
}

PartReading unread(std::string judge) {
    return PartReading{.judge = std::move(judge), .unread = "the engine holds no mission reading it"};
}

TEST(FlowScore, IsBsPreferenceAPartWhenBothOrdersAgreeMeanedOverJudgesThenParts) {
    const Pairing pairing{
        .panel = "eval-v9",
        .judges = {"commander", "infantry"},
        .parts = {// B second, then B first, preferred both times: B; a tie: 0. (1 + 0) / 2.
                  PairedPart{.part = part(1, 4, {0}, {0}),
                             .readings = {read("commander", Preference::second, Preference::first),
                                          read("infantry", Preference::tie, Preference::tie)}},
                  // Whatever came first, both times: a tie, and one inconsistent.
                  PairedPart{.part = part(5, 8, {1}, {1}),
                             .readings = {read("commander", Preference::first, Preference::first),
                                          unread("infantry")}},
                  PairedPart{.part = part(9, 12, {2}, {2}),
                             .readings = {unread("commander"), unread("infantry")}}}};
    const FlowScore score = flow_score(pairing);
    ASSERT_TRUE(score.b_preferred.has_value());
    EXPECT_THAT(*score.b_preferred, DoubleEq((0.5 + 0.0) / 2)) << "a part no judge read is left out";
    EXPECT_EQ(score.parts, 2U);
    EXPECT_EQ(score.inconsistent, 1U);

    const Pairing a_wins{
        .panel = "eval-v9",
        .judges = {"commander"},
        .parts = {PairedPart{.part = part(1, 4, {0}, {0}),
                             .readings = {read("commander", Preference::first, Preference::second)}}}};
    EXPECT_EQ(flow_score(a_wins).b_preferred, -1.0);
    EXPECT_EQ(flow_score(Pairing{.panel = "eval-v9", .judges = {"commander"}}).b_preferred, std::nullopt);
}

TEST(Pairing, RoundTripsThroughItsArtifact) {
    const Pairing pairing{
        .panel = "eval-v9",
        .judges = {"commander", "infantry"},
        .parts = {PairedPart{
            .part = part(4, 8, {0, 1}, {1}),
            .readings = {read("commander", Preference::tie, Preference::second), unread("infantry")}}}};
    EXPECT_EQ(unwrap(pairing_from_json(to_json(pairing))), pairing);
    json::Json broken = to_json(pairing);
    broken["parts"][0]["readings"][0].erase("b_first");
    EXPECT_ERRC(pairing_from_json(broken), Errc::invalid_argument)
        << "a reading with one order is no reading, and says not why";
}

// An entry of `video`'s replicate `replicate`, its job named by both.
ScoredEntry entry_of(const std::string& video, int replicate) {
    return ScoredEntry{
        .entry =
            RunEntry{.video = video, .replicate = replicate, .job = std::format("{}/{}", video, replicate)}};
}

TEST(PairsToRead, AreReplicateKOfAWithReplicateKOfBOfEachVideoBothRunsHave) {
    const std::vector<ScoredEntry> a{entry_of("lecture", 1), entry_of("lecture", 0), entry_of("podcast", 0),
                                     entry_of("talk", 0), entry_of("talk", 1)};
    const std::vector<ScoredEntry> b{entry_of("talk", 0), entry_of("lecture", 4), entry_of("news", 0),
                                     entry_of("lecture", 3)};
    const std::vector<VideoPairs> videos = pairs_to_read(a, b);
    ASSERT_EQ(videos.size(), 2U) << "the podcast A's alone, the news B's";
    EXPECT_EQ(videos[0].video, "lecture");
    EXPECT_THAT(videos[0].pairs,
                ElementsAre(std::pair{a[1].entry, b[3].entry}, std::pair{a[0].entry, b[1].entry}))
        << "in their numbers' order, however listed: an A/A run's 3 and 4 with the baseline's 0 and 1";
    EXPECT_EQ(videos[0].a_replicates, 2U);
    EXPECT_EQ(videos[1].video, "talk");
    EXPECT_THAT(videos[1].pairs, ElementsAre(std::pair{a[3].entry, b[0].entry})) << "the first of each";
    EXPECT_EQ(videos[1].a_replicates, 2U);
    EXPECT_EQ(videos[1].b_replicates, 1U);
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

TEST(FlowComparison, IsEachVideosMeanOverItsPairsReadUnderThePanel) {
    const Comparison flow =
        flow_comparison(panel(), {{"lecture", {1.0, 0.0, 0.5}}, {"podcast", {-1.0}}, {"talk", {}}});
    EXPECT_EQ(flow.metric, "flow");
    EXPECT_EQ(flow.scorer, "judges@eval-v9");
    EXPECT_THAT(flow.differences,
                ElementsAre(Pair("lecture", DoubleEq(0.5)), Pair("podcast", DoubleEq(-1.0))))
        << "the talk: no pair read, no value";
    EXPECT_THAT(flow.difference, DoubleEq(-0.25));
    EXPECT_EQ(flow.a, std::nullopt);
}

ArticlesToCompare lecture_pair(std::string_view b = kArticle) {
    return ArticlesToCompare{.title = "Weighing radium",
                             .channel = "Science Hour",
                             .transcript = pipeline::testing::lecture(),
                             .a_ledger = lecture_atoms(),
                             .a = knowledge::Article{.markdown = std::string{kArticle}},
                             .b_ledger = lecture_atoms(),
                             .b = knowledge::Article{.markdown = std::string{b}}};
}

const library::Job kJob{
    .id = "01PAIRING", .source = "01A 01B", .tier = "eval-v9", .kind = library::JobKind::pairing};

Result<pipeline::StageOutput> compare_on(fixtures::Rig& rig, ArticlesToCompare articles = lecture_pair()) {
    const FlowStage stage{panel(), std::move(articles)};
    pipeline::StageContext context{.job = &kJob, .army = rig.raise(), .prompts = &prompts()};
    RR_TRY_ASSIGN(context.fingerprint, stage.fingerprint(context));
    return stage.run(context);
}

TEST(FlowStage, ReadsEveryPartByEveryJudgeInBothOrders) {
    fixtures::Rig rig{{}, fixtures::engines_serving()};
    const pipeline::StageOutput output = unwrap(compare_on(rig));
    EXPECT_THAT(output.findings, IsEmpty());
    const Pairing pairing = unwrap(pairing_from_json(output.artifact));
    ASSERT_EQ(pairing.parts.size(), 2U);
    EXPECT_EQ(pairing.parts[0].part, part(4, 8, {0, 1}, {0, 1}));
    for (const PairedPart& paired : pairing.parts) {
        ASSERT_EQ(paired.readings.size(), 2U);
        for (const PartReading& reading : paired.readings) {
            EXPECT_EQ(reading.a_first, Preference::tie) << "the same words: neither reads better";
            EXPECT_EQ(reading.b_first, Preference::tie);
        }
    }
    EXPECT_EQ(flow_score(pairing).b_preferred, 0.0);
    const std::vector<llm::ChatRequest> asked = rig.requests("eval_flow");
    EXPECT_EQ(asked.size(), 8U) << "two parts, two judges, two orders";
    for (const llm::ChatRequest& request : asked) {
        EXPECT_EQ(request.temperature, 0.0) << "greedy";
        EXPECT_EQ(request.thinking, false);
        EXPECT_THAT(request.messages.back().text(), Not(HasSubstr("[A"))) << "no marker";
    }
    // The army asks from several threads: the scales' request is found by
    // what it asks, never by when it came.
    const auto scales = std::ranges::find_if(asked, [](const llm::ChatRequest& request) {
        return request.messages.back().text().find("from 1:30 to 3:55") != std::string::npos;
    });
    ASSERT_NE(scales, asked.end());
    EXPECT_THAT(
        scales->messages.back().text(),
        HasSubstr("Which version of the stretch of the video from 1:30 to 3:55 would you rather hand to "
                  "a reader?\n\n<first>\n# Weighing radium\n\n## The scales\n\nThe NRC lent us 3 "
                  "scales for it.\n\n"))
        << "the title with the first part, its sections' headings and prose";
}

TEST(FlowStage, CountsAPreferenceOnlyWhenItSurvivesTheSwap) {
    fixtures::Rig rig{{}, fixtures::engines_serving()};
    const Pairing pairing =
        unwrap(pairing_from_json(unwrap(compare_on(rig, lecture_pair(kShorterGlow))).artifact));
    ASSERT_EQ(pairing.parts.size(), 2U);
    for (const PartReading& reading : pairing.parts[1].readings) {
        EXPECT_EQ(reading.a_first, Preference::second) << "the oracle prefers the shorter, B";
        EXPECT_EQ(reading.b_first, Preference::first);
    }
    EXPECT_EQ(flow_score(pairing).b_preferred, 0.5) << "a tie, then B by both judges";

    fixtures::Rig first{
        {oracle::edit("always the first", oracle::When{.kind = "eval_flow", .model = "gemma-infantry"},
                      [](const std::string& /*answer*/) {
                          return std::string{R"({"reasons": "It came first.", "better": "first"})"};
                      })},
        fixtures::engines_serving()};
    const FlowScore biased =
        flow_score(unwrap(pairing_from_json(unwrap(compare_on(first, lecture_pair(kShorterGlow))).artifact)));
    EXPECT_EQ(biased.inconsistent, 2U) << "the infantry's two parts, each preferring whatever came first";
    EXPECT_EQ(biased.b_preferred, 0.25) << "the glow: B by the commander, a tie by the infantry";
}

TEST(FlowStage, AReadingOneOfWhoseOrdersNoAnswerSettledIsUnread) {
    // The infantry's answers on the scales with B first, never in the schema.
    fixtures::Rig rig{{oracle::edit("no such preference",
                                    oracle::When{.mission = "S4-S8, B first, by the infantry", .calls = 0},
                                    [](const std::string& /*answer*/) {
                                        return std::string{R"({"reasons": "Both.", "better": "both"})"};
                                    })},
                      fixtures::engines_serving()};
    const pipeline::StageOutput output = unwrap(compare_on(rig));
    const Pairing pairing = unwrap(pairing_from_json(output.artifact));
    const PartReading& half = pairing.parts[0].readings[1];
    EXPECT_EQ(half.a_first, std::nullopt)
        << "its A-first order was answered, but a preference must survive the swap";
    EXPECT_EQ(half.b_first, std::nullopt);
    EXPECT_FALSE(half.unread.empty());
    EXPECT_THAT(output.findings, ElementsAre(HasSubstr("S4 to S8: the infantry could not read it")));
    EXPECT_EQ(pairing.parts[0].readings[0].a_first, Preference::tie) << "the commander read it";
    EXPECT_EQ(pairing.parts[1].readings[1].a_first, Preference::tie) << "and the infantry the glow";
}

TEST(FlowStage, LeavesUnreadAPartAJudgesEngineCannotHold) {
    fixtures::Rig narrow{{}, fixtures::engines_serving(fixtures::kQwenFile, fixtures::kGemmaFile, 8192, 600)};
    const pipeline::StageOutput output = unwrap(compare_on(narrow));
    const Pairing pairing = unwrap(pairing_from_json(output.artifact));
    EXPECT_THAT(pairing.parts[0].readings[1].unread,
                HasSubstr("the infantry's engine holds no mission reading it"));
    EXPECT_FALSE(pairing.parts[0].readings[1].a_first.has_value());
    EXPECT_THAT(output.findings,
                ::testing::Contains(HasSubstr("S4 to S8: the infantry's engine holds no mission")));
}

TEST(FlowStage, ItsKeyHoldsBothArticlesAndThePanel) {
    const auto key = [&](ArticlesToCompare articles,
                         std::vector<agents::Engine> engines = fixtures::engines_serving()) {
        fixtures::Rig rig{{}, std::move(engines)};
        const FlowStage stage{panel(), std::move(articles)};
        const pipeline::StageContext context{.job = &kJob, .army = rig.raise(), .prompts = &prompts()};
        return unwrap(stage.fingerprint(context));
    };
    const json::Json same = key(lecture_pair());
    EXPECT_EQ(key(lecture_pair()), same);
    EXPECT_NE(key(lecture_pair(kShorterGlow)), same) << "B's article changed";
    ArticlesToCompare retitled = lecture_pair();
    retitled.title = "Weighing polonium";
    EXPECT_NE(key(retitled), same) << "the prompt shows the title";
    EXPECT_NE(
        key(lecture_pair(), fixtures::engines_serving(fixtures::kQwenFile, fixtures::kGemmaFile, 8192, 2048)),
        same)
        << "how much an engine holds decides which parts are read";
    const FlowStage stage{panel(), lecture_pair()};
    EXPECT_ERRC(stage.fingerprint(pipeline::StageContext{.job = &kJob, .prompts = &prompts()}),
                Errc::needs_user)
        << "no pod, no judges";
}

// Pairings on a real library: article jobs done on the lecture, their
// artifacts committed as a job done has them.
class Pairings : public ::testing::Test {
protected:
    // An article job done, its article `markdown`, of `transcript`.
    [[nodiscard]] library::Job done_article(
        const std::string& markdown = pipeline::testing::oracle_article(),
        const knowledge::Transcript& transcript = pipeline::testing::lecture()) {
        library::Job job = unwrap(jobs_.create("talk.vtt", "max", 0));
        const auto commit = [&](std::string_view stage, json::Json artifact) {
            clock_.advance(1s);
            // A key an artifact: jobs with equal artifacts share them.
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
        commit(pipeline::TranscriptStage::kName, knowledge::to_json(transcript));
        commit(pipeline::AtomizeStage::kName, knowledge::to_json(pipeline::testing::lecture_ledger()));
        commit(pipeline::SeamsStage::kName, knowledge::to_json(knowledge::Article{.markdown = markdown}));
        job.status = library::JobStatus::done;
        job.title = "Weighing radium";
        return unwrap(jobs_.save(job));
    }

    [[nodiscard]] Result<PairingRun> pair(fixtures::Rig& rig, const library::Job& a, const library::Job& b) {
        RR_TRY_ASSIGN(ArticlesToCompare read, articles_to_compare(a, b, store_));
        RR_TRY_ASSIGN(library::Job job, start_pairing(database_, clock_, a, b, panel()));
        return run_pairing(std::move(job), panel(), std::move(read), database_, clock_,
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

// The oracle's article of the lecture, its question asked in fewer words.
std::string shorter_question() {
    std::string markdown = pipeline::testing::oracle_article();
    const std::string asked = "Why does it glow?";
    const std::size_t at = markdown.find(asked);
    EXPECT_NE(at, std::string::npos);
    return markdown.replace(at, asked.size(), "Why?");
}

TEST_F(Pairings, TwoArticlesHaveOnePairingAPanelInTheOrderCompared) {
    const library::Job a = done_article();
    const library::Job b = done_article();
    const library::Job pairing = unwrap(start_pairing(database(), clock(), a, b, panel()));
    EXPECT_EQ(pairing.kind, library::JobKind::pairing);
    EXPECT_EQ(pairing.source, std::format("{} {}", a.id, b.id)) << "the jobs it compares, A's first";
    EXPECT_EQ(pairing.tier, "eval-v9") << "the panel that reads them";
    EXPECT_EQ(pairing.salt, 0);
    EXPECT_EQ(pairing.title, "Weighing radium") << "listed by the video";
    EXPECT_EQ(unwrap(start_pairing(database(), clock(), a, b, panel())).id, pairing.id) << "taken up";
    EXPECT_NE(unwrap(start_pairing(database(), clock(), b, a, panel())).id, pairing.id)
        << "B against A is another comparison";
    Panel other = panel();
    other.name = "eval-v10";
    EXPECT_NE(unwrap(start_pairing(database(), clock(), a, b, other)).id, pairing.id) << "each panel its own";
}

TEST_F(Pairings, APairingDoneIsScoredAndAPairUnchangedIsNeverReadTwice) {
    const library::Job a = done_article();
    fixtures::Rig rig{{}, fixtures::engines_serving()};
    const PairingRun alike = unwrap(pair(rig, a, done_article()));
    EXPECT_EQ(alike.run.job.status, library::JobStatus::done);
    ASSERT_TRUE(alike.score.has_value());
    EXPECT_EQ(alike.score->b_preferred, 0.0) << "the same words: every part a tie";
    EXPECT_EQ(alike.score->parts, 3U) << "one outline: a part a section";
    EXPECT_EQ(rig.log.requests().size(), 12U) << "three parts, two judges, two orders";

    const library::Job shorter = done_article(shorter_question());
    const PairingRun preferred = unwrap(pair(rig, a, shorter));
    ASSERT_TRUE(preferred.score.has_value());
    EXPECT_THAT(preferred.score->b_preferred, Optional(DoubleEq(1.0 / 3)))
        << "the part asking the question B's, by both judges in both orders; the others ties";
    const std::size_t asked = rig.log.requests().size();

    EXPECT_EQ(unwrap(pair(rig, a, shorter)).score, preferred.score);
    EXPECT_EQ(rig.log.requests().size(), asked) << "its pairing found done";
    EXPECT_EQ(unwrap(pair(rig, done_article(), done_article(shorter_question()))).score, preferred.score);
    EXPECT_EQ(rig.log.requests().size(), asked)
        << "other jobs' articles, the same: their pairing's stage found in the cache";
}

TEST_F(Pairings, APairingWithoutThePodNeedsYouAndIsNotScored) {
    const library::Job a = done_article();
    const library::Job b = done_article();
    library::Job job = unwrap(start_pairing(database(), clock(), a, b, panel()));
    const PairingRun waiting =
        unwrap(run_pairing(std::move(job), panel(), unwrap(articles_to_compare(a, b, store())), database(),
                           clock(), pipeline::StageContext{.prompts = &prompts(), .work_directory = work()}));
    EXPECT_EQ(waiting.run.job.status, library::JobStatus::needs_user);
    EXPECT_EQ(waiting.score, std::nullopt);
}

TEST_F(Pairings, OnlyTwoArticlesOfOneTranscriptAreCompared) {
    const library::Job a = done_article();
    const ArticlesToCompare read = unwrap(articles_to_compare(a, done_article(shorter_question()), store()));
    EXPECT_EQ(read.title, "Weighing radium");
    EXPECT_EQ(read.channel, "Science Hour");
    EXPECT_EQ(read.a.markdown, pipeline::testing::oracle_article());
    EXPECT_EQ(read.b.markdown, shorter_question());

    std::vector<std::string> texts = pipeline::testing::lecture_texts();
    texts.back() = "Thanks for watching, all.";
    const auto other = articles_to_compare(
        a, done_article(pipeline::testing::oracle_article(), pipeline::testing::transcript_of(texts)),
        store());
    EXPECT_ERRC(other, Errc::invalid_argument);
    EXPECT_THAT(other.error().message(), HasSubstr("read other transcripts"));
    const library::Job started = unwrap(jobs().create("talk.vtt", "max", 0));
    EXPECT_ERRC(articles_to_compare(a, started, store()), Errc::invalid_argument) << "no article";
}

}  // namespace
}  // namespace rr::eval

// The annotate stage on an army whose models are the oracle, with the faults
// real models make where a test says; and an annotation's job, from its
// snapshot to the gold file a person corrects.
#include <algorithm>
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

#include <rr/core/strings.hpp>
#include <rr/eval/annotate.hpp>
#include <rr/eval/sheet.hpp>
#include <rr/eval/snapshot.hpp>
#include <rr/library/library.hpp>
#include <rr/llm/memo.hpp>
#include <rr/prompt/library.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

#include "fixtures.hpp"

namespace rr::eval {
namespace {

using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using namespace std::chrono_literals;
using testkit::unwrap;

// A short talk. The oracle notes each sentence of five words or more, core
// when it holds a digit; the other family each of three or four.
std::vector<fixtures::Said> talk() {
    return {{"Welcome back.", 0s, 2s},                                   // S1: too short to note
            {"Today we weigh the radium salts again.", 2s, 6s},          // S2
            {"Marie Curie measured 7 samples in 1902.", 6s, 10s},        // S3: core
            {"That took months.", 10s, 12s},                             // S4: the other family's
            {"The samples glowed faintly in the dark shed.", 12s, 16s},  // S5
            {"Thank you.", 16s, 18s}};                                   // S6: too short
}

// The facts of `silver`, a line each: "S3 core found 3: Marie Curie …".
std::vector<std::string> noted(const Silver& silver) {
    std::vector<std::string> lines;
    lines.reserve(silver.facts.size());
    for (const SilverFact& fact : silver.facts) {
        std::string sentences;
        for (const knowledge::SentenceId id : fact.sentences) {
            sentences += sentences.empty() ? id.str() : "," + id.str();
        }
        lines.push_back(
            std::format("{} {} found {}: {}", sentences, to_string(fact.importance), fact.found, fact.quote));
    }
    return lines;
}

// What the last message of `request` holds between "<tag>\n" and
// "\n</tag>"; empty when it holds none (a repair's instruction).
std::string block_of(const llm::ChatRequest& request, std::string_view tag) {
    const std::string task = request.messages.back().text();
    const std::string open = std::format("<{}>\n", tag);
    const std::size_t begin = task.find(open);
    const std::size_t end = task.find(std::format("\n</{}>", tag));
    return begin == std::string::npos || end == std::string::npos
               ? std::string{}
               : task.substr(begin + open.size(), end - begin - open.size());
}

// The sentences a request asks to annotate: the S# of each line of its
// <sentences> block.
std::vector<std::string> asked_about(const llm::ChatRequest& request) {
    const std::string lines = block_of(request, "sentences");
    std::vector<std::string> ids;
    if (lines.empty()) {
        return ids;
    }
    for (const std::string_view line : split(lines, '\n')) {
        ids.emplace_back(line.substr(0, line.find(' ')));
    }
    return ids;
}

// The sentences the notes of a request cite: "- [S12, S13] claim".
std::vector<std::string> cited_in_notes(const llm::ChatRequest& request) {
    const std::string notes = block_of(request, "found");
    std::vector<std::string> ids;
    for (const std::string_view line : split(notes, '\n')) {
        const std::size_t open = line.find('[');
        const std::size_t close = line.find(']');
        if (open == std::string_view::npos || close == std::string_view::npos) {
            continue;  // "(none)"
        }
        for (const std::string_view id : split(line.substr(open + 1, close - open - 1), ',')) {
            ids.emplace_back(trim(id));
        }
    }
    return ids;
}

class AnnotateStageTest : public ::testing::Test {
protected:
    // The stage's fingerprint and output on the source and transcript of
    // `said`, read by `army`.
    Result<pipeline::StageOutput> run(const std::vector<fixtures::Said>& said, pipeline::RaiseArmy army) {
        pipeline::StageContext context{.job = &job_, .army = std::move(army), .prompts = &prompts_};
        context.inputs.emplace("source", to_json(fixtures::source_of(fixtures::vtt_of(said))));
        context.inputs.emplace("transcript", knowledge::to_json(fixtures::transcript_of(said)));
        RR_TRY_ASSIGN(context.fingerprint, stage_.fingerprint(context));
        fingerprint_ = context.fingerprint;
        return stage_.run(context);
    }

    static Silver silver_of(const pipeline::StageOutput& output) {
        return unwrap(silver_from_json(output.artifact));
    }

    [[nodiscard]] const json::Json& fingerprint() const { return fingerprint_; }
    [[nodiscard]] library::Job& job() { return job_; }

private:
    library::Job job_{.id = "J1"};
    const prompt::Library prompts_ = unwrap(prompt::Library::embedded());
    const AnnotateStage stage_;
    json::Json fingerprint_;
};

TEST_F(AnnotateStageTest, TheCommanderDraftsThreeTimesAndTheOtherFamilyAddsWhatTheyMissed) {
    fixtures::Rig rig;
    const pipeline::StageOutput output = unwrap(run(talk(), rig.raise()));
    EXPECT_THAT(output.findings, IsEmpty());
    EXPECT_TRUE(output.reusable);
    const Silver silver = silver_of(output);
    EXPECT_THAT(noted(silver),
                ElementsAre("S2 detail found 3: Today we weigh the radium salts again.",
                            "S3 core found 3: Marie Curie measured 7 samples in 1902.",
                            "S4 detail found 0: That took months.",
                            "S5 detail found 3: The samples glowed faintly in the dark shed."));
    ASSERT_EQ(silver.facts.size(), 4U);
    EXPECT_EQ(silver.facts[1].start, 6s) << "when its sentences were said";
    EXPECT_EQ(silver.facts[1].end, 10s);
    EXPECT_EQ(silver.drafted_by, "annotate@1 \u00D73 on qwen-commander; annotate_more@1 on gemma-infantry");

    const std::vector<llm::ChatRequest> drafts = rig.requests("annotate");
    ASSERT_EQ(drafts.size(), 1U) << "one window, its three samples drawn from one reading of the prompt";
    EXPECT_EQ(drafts[0].model, "qwen-commander");
    EXPECT_EQ(drafts[0].n, 3);
    EXPECT_EQ(drafts[0].thinking, true);
    EXPECT_EQ(drafts[0].seed, llm::derived_seed(0, "annotate", "annotate-1", 0)) << "from the job's salt";
    EXPECT_THAT(drafts[0].messages.front().text(), HasSubstr("the video \"On radium\", by Science Hour."));
    EXPECT_THAT(
        drafts[0].messages.back().text(),
        HasSubstr(
            "<sentences>\nS1 [0:00] Welcome back.\nS2 [0:02] Today we weigh the radium salts again.\n"));

    const std::vector<llm::ChatRequest> more = rig.requests("annotate_more");
    ASSERT_EQ(more.size(), 1U);
    EXPECT_EQ(more[0].model, "gemma-infantry") << "the other family";
    EXPECT_FALSE(more[0].thinking.value_or(false));
    EXPECT_THAT(
        more[0].messages.back().text(),
        HasSubstr("<found>\n- [S2] Today we weigh the radium salts again.\n- [S3] Marie Curie measured 7 "
                  "samples in 1902.\n- [S5] The samples glowed faintly in the dark shed.\n</found>"))
        << "the facts found so far, by their sentences";
}

TEST_F(AnnotateStageTest, ItsKeyNamesThePromptsTheModelsAndWhatTheyHold) {
    fixtures::Rig rig;
    ASSERT_OK(run(talk(), rig.raise()));
    EXPECT_EQ(fingerprint().at("prompts").at("annotate").at("label"), "annotate@1");
    EXPECT_EQ(fingerprint().at("prompts").at("annotate_more").at("label"), "annotate_more@1");
    EXPECT_EQ(fingerprint().at("prompts").at("annotate").at("hash").get<std::string>().size(), 64U);
    EXPECT_EQ(fingerprint().at("army").size(), 2U);
    EXPECT_EQ(fingerprint().at("windows"), unwrap(json::parse(R"({
        "annotate": {"bytes": 6486, "holds": 8192},
        "more": {"bytes": 2808, "holds": 4096},
        "context": {"sentences": 3, "bytes": 1000}})")))
        << "how the transcript is cut depends on what the engines hold";
    EXPECT_EQ(fingerprint().at("samples"), 3);
}

TEST_F(AnnotateStageTest, TheJobsSaltDrawsItsSamples) {
    job().salt = 7;
    fixtures::Rig rig;
    ASSERT_OK(run(talk(), rig.raise()));
    const std::vector<llm::ChatRequest> drafts = rig.requests("annotate");
    ASSERT_EQ(drafts.size(), 1U);
    EXPECT_EQ(drafts[0].seed, llm::derived_seed(7, "annotate", "annotate-1", 0))
        << "a replicate draws new samples";
}

TEST_F(AnnotateStageTest, WithoutAPodThereIsNothingToDraftWith) {
    const auto output = run(talk(), {});
    EXPECT_ERRC(output, Errc::needs_user);
    EXPECT_THAT(output.error().message(), HasSubstr("no pod is configured"));
}

TEST_F(AnnotateStageTest, AWindowGivesUpSentencesUntilItsEngineHoldsIt) {
    // Thirty sentences of about 330 bytes, and a commander that holds 3,800
    // tokens: a window filled to its bytes is more than it holds, with its
    // context on both sides, so it gives up its last sentence.
    std::vector<fixtures::Said> said;
    for (int index = 0; index < 30; ++index) {
        const auto start = std::chrono::seconds{20 * index};
        said.push_back(
            {std::format("Part {}: in the shed the samples of radium salts glowed faintly while the team "
                         "measured them again and again, and in the shed the samples of radium salts "
                         "glowed faintly while the team measured them again and again, and in the shed "
                         "the samples of radium salts glowed faintly while the team measured them, and "
                         "wrote down what they saw.",
                         index + 1),
             start, start + 20s});
    }
    fixtures::Rig rig{{}, fixtures::engines(3'800)};
    const pipeline::StageOutput output = unwrap(run(said, rig.raise()));
    EXPECT_THAT(output.findings, IsEmpty());
    for (const std::string& line : noted(silver_of(output))) {
        EXPECT_THAT(line, HasSubstr("found 3")) << "every sentence drafted three times";
    }
    EXPECT_EQ(silver_of(output).facts.size(), 30U);

    // Every sentence asked about once, window after window. The windows'
    // missions run side by side: their requests come in any order.
    std::vector<llm::ChatRequest> drafts = rig.requests("annotate");
    std::ranges::sort(drafts, {}, [](const llm::ChatRequest& request) {
        return std::stoi(request.tag.mission_id.substr(std::string_view{"annotate-"}.size()));
    });
    std::vector<std::string> asked;
    for (const llm::ChatRequest& request : drafts) {
        for (std::string& id : asked_about(request)) {
            asked.push_back(std::move(id));
        }
    }
    std::vector<std::string> all;
    for (int number = 1; number <= 30; ++number) {
        all.push_back(std::format("S{}", number));
    }
    EXPECT_EQ(asked, all);

    // The commander may answer twice what it reads, after its thinking
    // (500 tokens and 4 of closing words): its lines and their newlines, at
    // three bytes a token.
    for (const llm::ChatRequest& request : drafts) {
        const auto read = static_cast<int>(block_of(request, "sentences").size() + 1) / 3;
        EXPECT_EQ(request.max_tokens, std::clamp(read * 2, 1'024, 8'192) + 504) << request.tag.mission_id;
    }

    // The other family is shown the facts found in its own window, and no others.
    const std::vector<llm::ChatRequest> more = rig.requests("annotate_more");
    ASSERT_GE(more.size(), 2U);
    for (const llm::ChatRequest& request : more) {
        const std::vector<std::string> own = asked_about(request);
        for (const std::string& id : cited_in_notes(request)) {
            EXPECT_THAT(own, Contains(id)) << request.tag.mission_id;
        }
        EXPECT_FALSE(cited_in_notes(request).empty()) << "every sentence was noted";
    }

    // More windows than the bytes alone would have made.
    const knowledge::Transcript transcript = fixtures::transcript_of(said);
    const auto bytes = fingerprint().at("windows").at("annotate").at("bytes").get<std::size_t>();
    std::size_t filled = 0;
    for (std::size_t begin = 0; begin < transcript.sentences.size();
         begin = pipeline::window_from(transcript, begin, bytes).end) {
        ++filled;
    }
    EXPECT_GT(drafts.size(), filled);
}

TEST_F(AnnotateStageTest, ASentenceTooLongForItsEngineEvenAloneIsAFinding) {
    std::vector<fixtures::Said> said = talk();
    std::string endless = "Marie Curie measured";
    for (int sample = 0; sample < 900; ++sample) {
        endless += " 7 samples";
    }
    said[2] = {endless + ".", 6s, 40s};
    for (std::size_t index = 3; index < said.size(); ++index) {
        said[index].start += 30s;
        said[index].end += 30s;
    }
    fixtures::Rig rig;
    const pipeline::StageOutput output = unwrap(run(said, rig.raise()));
    EXPECT_THAT(
        output.findings,
        ElementsAre("the sentence S3 (0:06 to 0:40) has no draft: even alone, with its context and an "
                    "answer, it is more than qwen-commander holds at once",
                    "the sentence S3 (0:06 to 0:40) has no draft: even alone, with its context and an "
                    "answer, it is more than gemma-infantry holds at once"));
    EXPECT_THAT(noted(silver_of(output)),
                ElementsAre(HasSubstr("S2 detail found 3"), HasSubstr("S4 detail found 0"),
                            HasSubstr("S5 detail found 3")))
        << "the other sentences are drafted";
}

TEST_F(AnnotateStageTest, AWindowTheModelsDeclineIsAFindingAndTheOtherFamilyStillReadsIt) {
    fixtures::Rig rig{{oracle::refusal(oracle::When{.mission = "annotate-1", .calls = 0})}};
    const pipeline::StageOutput output = unwrap(run(talk(), rig.raise()));
    EXPECT_THAT(
        output.findings,
        ElementsAre("the sentences S1\u2013S6 (0:00 to 0:18) have no draft: the models declined to annotate "
                    "them"));
    EXPECT_TRUE(output.reusable) << "they would decline again";
    EXPECT_THAT(noted(silver_of(output)),
                ElementsAre("S2 detail found 0: Today we weigh the radium salts again.",
                            "S3 detail found 0: Marie Curie measured 7 samples in 1902.",
                            "S4 detail found 0: That took months.",
                            "S5 detail found 0: The samples glowed faintly in the dark "
                            "shed."));
    const std::vector<llm::ChatRequest> more = rig.requests("annotate_more");
    ASSERT_EQ(more.size(), 1U);
    EXPECT_THAT(more[0].messages.back().text(), HasSubstr("<found>\n(none)\n</found>"));
}

TEST_F(AnnotateStageTest, AQuoteNotSaidIsSentBackAndRepaired) {
    fixtures::Rig rig{{oracle::edit(
        "misquote", oracle::When{.mission = "annotate-1", .calls = 1}, [](const std::string& answer) {
            return replace_all(answer, R"("quote":"Marie Curie)", R"("quote":"Pierre Curie)");
        })}};
    const pipeline::StageOutput output = unwrap(run(talk(), rig.raise()));
    EXPECT_THAT(output.findings, IsEmpty());
    EXPECT_THAT(noted(silver_of(output)),
                Contains("S3 core found 3: Marie Curie measured 7 samples in 1902."))
        << "each sample repaired";
    std::vector<std::string> sent_back;
    for (const llm::ChatRequest& request : rig.requests("annotate")) {
        sent_back.push_back(request.messages.back().text());
    }
    EXPECT_THAT(sent_back,
                Contains(HasSubstr("fact 2: \"Pierre Curie measured 7 samples in 1902.\" is not what S3 "
                                   "says, word for word: copy the quote from it")));
}

TEST_F(AnnotateStageTest, ACeilingStopsTheDraftsAndMakesTheResultThisJobsAlone) {
    fixtures::Rig rig{{}, fixtures::engines(), agents::Ceilings{.completion_tokens = 1}};
    const pipeline::StageOutput output = unwrap(run(talk(), rig.raise()));
    EXPECT_FALSE(output.reusable) << "a job allowed more would have drafted more";
    EXPECT_THAT(output.findings,
                ElementsAre("the sentences S1\u2013S6 (0:00 to 0:18) have no draft: the job reached its "
                            "completion_tokens ceiling first"));
    EXPECT_THAT(noted(silver_of(output)), Contains(HasSubstr("found 3")))
        << "what the commander drafted is kept";
}

// --- The job ----------------------------------------------------------------

class AnnotationJobTest : public ::testing::Test {
protected:
    AnnotationJobTest() {
        snapshot_ =
            unwrap(keep_snapshot(root_.path(), video_, to_json(fixtures::source_of(fixtures::vtt_of(talk()))),
                                 std::chrono::system_clock::time_point{}));
    }

    Result<library::Job> start(bool replace = false) {
        return start_annotation(root_.path(), video_, database_, clock_, replace);
    }

    Result<Annotated> run(library::Job job, pipeline::RaiseArmy army) {
        return run_annotation(
            root_.path(), video_, std::move(job), database_, clock_,
            pipeline::StageContext{
                .army = std::move(army), .prompts = &prompts_, .work_directory = root_ / "work"});
    }

    [[nodiscard]] std::filesystem::path gold_path() const { return gold_file(root_.path(), video_.id); }
    [[nodiscard]] const testkit::TempDir& root() const { return root_; }
    [[nodiscard]] const EvalVideo& video() const { return video_; }
    [[nodiscard]] const Snapshot& snapshot() const { return snapshot_; }
    [[nodiscard]] db::Database& database() { return database_; }
    [[nodiscard]] FakeClock& clock() { return clock_; }

private:
    testkit::TempDir root_{"rr_eval"};
    EvalVideo video_ = fixtures::video();
    FakeClock clock_;
    db::Database database_ = unwrap(library::open_database(root_ / "library.db"));
    const prompt::Library prompts_ = unwrap(prompt::Library::embedded());
    Snapshot snapshot_;
};

TEST_F(AnnotationJobTest, ADraftIsWrittenToTheGoldFileForAPersonToCorrect) {
    const library::Job job = unwrap(start());
    EXPECT_EQ(job.source, std::filesystem::absolute(snapshot_file(root().path(), video().id)).string())
        << "the snapshot, never YouTube";
    EXPECT_EQ(job.tier, "max");
    EXPECT_EQ(job.kind, library::JobKind::annotation)
        << "resumed by `rreport eval annotate`, never by `resume`";

    fixtures::Rig rig;
    const Annotated annotated = unwrap(run(job, rig.raise()));
    EXPECT_EQ(annotated.run.job.status, library::JobStatus::done);
    ASSERT_TRUE(annotated.gold.has_value());
    const Gold& gold = *annotated.gold;
    EXPECT_EQ(unwrap(parse_gold(root().read_file(gold_path()), "gold")), gold) << "as written";
    EXPECT_EQ(gold.video, "radium-talk");
    EXPECT_EQ(gold.snapshot, snapshot().hash);
    EXPECT_EQ(gold.status, GoldStatus::silver);
    EXPECT_EQ(gold.reviewer, "");
    EXPECT_EQ(gold.drafted_by, "annotate@1 \u00D73 on qwen-commander; annotate_more@1 on gemma-infantry");

    std::vector<std::string> facts;
    facts.reserve(gold.facts.size());
    for (const Fact& fact : gold.facts) {
        facts.push_back(std::format("{} {}\u2013{} {} found {}: {}", fact.id, clock_time(fact.start),
                                    clock_time(fact.end), to_string(fact.importance), fact.found,
                                    fact.quote));
    }
    EXPECT_THAT(
        facts, ElementsAre("F1 0:02\u20130:06 detail found 3: Today we weigh the radium salts again.",
                           "F2 0:06\u20130:10 core found 3: Marie Curie measured 7 samples in 1902.",
                           "F3 0:10\u20130:12 detail found 0: That took months.",
                           "F4 0:12\u20130:16 detail found 3: The samples glowed faintly in the dark shed."));
    EXPECT_THAT(gold_problems(gold, unwrap(caption_transcript(snapshot().source))), IsEmpty())
        << "every quote is said where its fact says";
}

TEST_F(AnnotationJobTest, ARerunTakesUpAnUnfinishedJob) {
    const library::Job first = unwrap(start());
    const Annotated waiting = unwrap(run(first, {}));
    EXPECT_EQ(waiting.run.job.status, library::JobStatus::needs_user) << "no pod to draft with";
    EXPECT_FALSE(waiting.gold.has_value());
    EXPECT_FALSE(std::filesystem::exists(gold_path()));

    const library::Job again = unwrap(start());
    EXPECT_EQ(again.id, first.id) << "taken up again: its memo answers every call it made";
    fixtures::Rig rig;
    EXPECT_EQ(unwrap(run(again, rig.raise())).run.job.status, library::JobStatus::done);
    EXPECT_TRUE(std::filesystem::exists(gold_path()));
}

TEST_F(AnnotationJobTest, ASnapshotGoneSinceTheJobStartedStopsIt) {
    const library::Job job = unwrap(start());
    std::filesystem::remove(snapshot_file(root().path(), video().id));
    fixtures::Rig rig;
    EXPECT_ERRC(run(job, rig.raise()), Errc::not_found);
    EXPECT_FALSE(std::filesystem::exists(gold_path()));
}

TEST_F(AnnotationJobTest, AFailedJobIsNotTakenUpAgain) {
    library::Job failed = unwrap(start());
    failed.status = library::JobStatus::failed;
    library::Jobs jobs{database(), clock()};
    ASSERT_OK(jobs.save(failed));
    EXPECT_NE(unwrap(start()).id, failed.id) << "what failed would fail again";
}

TEST_F(AnnotationJobTest, AnArticleOfTheSnapshotIsNotTakenUp) {
    library::Jobs jobs{database(), clock()};
    const library::Job article = unwrap(
        jobs.create(std::filesystem::absolute(snapshot_file(root().path(), video().id)).string(), "max", 0));
    EXPECT_NE(unwrap(start()).id, article.id) << "`rreport ingest` of the snapshot runs other stages";
}

TEST_F(AnnotationJobTest, ADraftReplacesSilverOnlyWhenAskedAndNeverGold) {
    fixtures::Rig rig;
    const library::Job first = unwrap(start());
    ASSERT_OK(run(first, rig.raise()));

    const auto again = start();
    EXPECT_ERRC(again, Errc::conflict);
    EXPECT_THAT(again.error().message(),
                HasSubstr("exists: --replace drafts it again, and loses what was corrected in it"));
    EXPECT_NE(unwrap(start(true)).id, first.id) << "a new job: the first is done";

    Gold reviewed = unwrap(parse_gold(root().read_file(gold_path()), "gold"));
    reviewed.status = GoldStatus::gold;
    reviewed.reviewer = "Ada";
    (void)root().write_file(gold_path(), to_toml(reviewed));
    const auto never = start(true);
    EXPECT_ERRC(never, Errc::conflict);
    EXPECT_THAT(never.error().message(), HasSubstr("is reviewed gold: a draft never replaces it"));
}

TEST_F(AnnotationJobTest, AGoldFileThatCannotBeReadIsNeverReplaced) {
    (void)root().write_file(gold_path(), "status = ");
    EXPECT_ERRC(start(true), Errc::invalid_argument) << "it may be a person's work half done";
}

TEST_F(AnnotationJobTest, AVideoNeverFetchedCannotBeAnnotated) {
    EvalVideo other = video();
    other.id = "other-talk";
    const auto missing = start_annotation(root().path(), other, database(), clock(), false);
    EXPECT_ERRC(missing, Errc::not_found);
    EXPECT_THAT(missing.error().message(), HasSubstr("`rreport eval fetch other-talk` reads it first"));
}

}  // namespace
}  // namespace rr::eval

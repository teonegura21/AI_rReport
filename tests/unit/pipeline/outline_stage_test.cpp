// The outline stage on a synthetic lecture: sections from the commander's
// outlines, from the creator's chapters, or from pauses; each described, and
// the video summed up; with the faults real models make injected where a
// test says.
#include <rr/pipeline/outline_stage.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/agents/sizing.hpp>
#include <rr/core/clock.hpp>
#include <rr/core/strings.hpp>
#include <rr/knowledge/outline.hpp>
#include <rr/knowledge/transcript.hpp>
#include <rr/oracle/answers.hpp>
#include <rr/oracle/oracle.hpp>
#include <rr/pipeline/source.hpp>
#include <rr/testkit/result.hpp>

namespace rr::pipeline {
namespace {

using knowledge::Genre;
using knowledge::LexiconTerm;
using knowledge::Section;
using knowledge::SectionsFrom;
using knowledge::SentenceId;
using knowledge::Speaker;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using testkit::unwrap;

// --- The lecture -------------------------------------------------------------------

// Thirty sentences, one every 30 s, each said for 25 s: fifteen minutes.
// The talk turns at S8, S16 and S24, where the oracle starts a section; Ada
// says who she is in S2, and S16 names the NRC.
std::vector<std::string> lecture_texts() {
    std::vector<std::string> texts;
    texts.reserve(30);
    for (int number = 1; number <= 30; ++number) {
        texts.push_back(std::format(
            "Sample {} glowed faintly in the dark, and Ada wrote down what the scales said.", number));
    }
    texts[0] = "Welcome back to the shed, everyone.";
    texts[1] = "I'm Ada, and today we weigh radium.";
    texts[7] = "Now, the scales we weigh it with.";
    texts[15] = "Next, the samples from the NRC lab.";
    texts[23] = "Now, what the numbers mean.";
    texts[29] = "Thank you for listening.";
    return texts;
}

knowledge::Transcript transcript_of(const std::vector<std::string>& texts) {
    knowledge::Transcript transcript;
    transcript.sentences.reserve(texts.size());
    for (std::size_t index = 0; index < texts.size(); ++index) {
        const Millis start{static_cast<std::int64_t>(index) * 30'000};
        transcript.sentences.push_back(
            knowledge::Sentence{.id = SentenceId{static_cast<std::uint32_t>(index + 1)},
                                .text = texts[index],
                                .span = unwrap(TimeSpan::make(start, start + Millis{25'000}))});
    }
    return transcript;
}

knowledge::Transcript lecture() {
    return transcript_of(lecture_texts());
}

// A lecture of 2:09:55: 260 sentences, the talk turning every ten.
knowledge::Transcript long_lecture() {
    std::vector<std::string> texts;
    texts.reserve(260);
    for (int number = 1; number <= 260; ++number) {
        texts.push_back(number % 10 == 1
                            ? std::format("Now, part {} of the weighing.", (number / 10) + 1)
                            : std::format("Sample {} glowed faintly in the dark, and Ada wrote down "
                                          "what the scales said.",
                                          number));
    }
    return transcript_of(texts);
}

media::Chapter chapter(std::string title, int start, int end) {
    return media::Chapter{.title = std::move(title),
                          .span = unwrap(TimeSpan::make(Millis{start * 1'000}, Millis{end * 1'000}))};
}

Source source_with(std::vector<media::Chapter> chapters = {}) {
    return Source{.origin = Origin::web,
                  .video = media::Video{.id = "abc123XYZ_0",
                                        .title = "Weighing radium",
                                        .description = "A lecture from the shed.",
                                        .channel = "Science Hour",
                                        .chapters = std::move(chapters)},
                  .captions = SourceCaptions{.language = "en",
                                             .kind = media::CaptionKind::manual,
                                             .format = transcript::CaptionFormat::vtt,
                                             .content = "WEBVTT\n"}};
}

// What the oracle makes of the lecture: four sections, where the talk turns.
std::vector<Section> lecture_sections() {
    return {Section{.first = SentenceId{1},
                    .last = SentenceId{7},
                    .title = "Welcome back to the shed",
                    .gist = "Welcome back to the shed, everyone."},
            Section{.first = SentenceId{8},
                    .last = SentenceId{15},
                    .title = "Now, the scales we weigh",
                    .gist = "Now, the scales we weigh it with."},
            Section{.first = SentenceId{16},
                    .last = SentenceId{23},
                    .title = "Next, the samples from the",
                    .gist = "Next, the samples from the NRC lab."},
            Section{.first = SentenceId{24},
                    .last = SentenceId{30},
                    .title = "Now, what the numbers mean",
                    .gist = "Now, what the numbers mean."}};
}

knowledge::Brief lecture_brief() {
    return knowledge::Brief{
        .genre = Genre::talk,
        .speakers = {Speaker{.name = "Ada"}},
        .lexicon = {LexiconTerm{.term = "NRC"}},
        .gist = "A video in 4 sections, from Welcome back to the shed to Now, what the numbers mean."};
}

// --- The army ------------------------------------------------------------------------

std::vector<agents::Engine> engines(int commander_holds = 32'768, int commander_slots = 4) {
    const llm::SamplingPreset direct{.temperature = 1.0, .top_p = 0.95, .top_k = 64};
    const llm::SamplingPreset thinking{
        .temperature = 1.0,
        .top_p = 0.95,
        .top_k = 64,
        .thinking = true,
        .thinking_budget = llm::ThinkingBudget{.tokens = 500, .closing = "Answer now."}};
    return {agents::Engine{.model = "qwen-commander",
                           .checksum = "sha-q",
                           .roles = {"commander"},
                           .family = "qwen",
                           .slots = commander_slots,
                           .context = commander_holds * commander_slots,
                           .ctx_per_slot = commander_holds,
                           .direct = direct,
                           .thinking = thinking},
            agents::Engine{.model = "gemma-infantry",
                           .checksum = "sha-g",
                           .roles = {"infantry"},
                           .family = "gemma",
                           .slots = 4,
                           .context = 4 * 32'768,
                           .ctx_per_slot = 32'768,
                           .direct = direct,
                           .thinking = thinking}};
}

// An army whose models are the oracle, with `faults` made where they say.
class Rig {
public:
    explicit Rig(std::vector<oracle::Fault> faults = {}, std::vector<agents::Engine> engines_used = engines(),
                 agents::Ceilings ceilings = {})
        : faulty_(oracle_, std::move(faults))
        , recorder_(faulty_) {
        oracle::teach_stages(oracle_);
        army_.emplace(unwrap(agents::Army::create(
            agents::Army::Options{.engines = std::move(engines_used), .job_id = "J1", .ceilings = ceilings},
            recorder_, clock_)));
    }

    [[nodiscard]] RaiseArmy raise() {
        return [this]() -> Result<agents::Army*> { return &*army_; };
    }

    // The requests of the missions of `kind`, by mission (a squad's missions
    // run at once, in any order), each mission's in the order sent.
    [[nodiscard]] std::vector<llm::ChatRequest> requests(std::string_view kind) const {
        std::vector<llm::ChatRequest> found;
        for (const llm::ChatRequest& request : recorder_.requests()) {
            if (request.tag.purpose == kind) {
                found.push_back(request);
            }
        }
        std::ranges::stable_sort(found, {},
                                 [](const llm::ChatRequest& request) { return request.tag.mission_id; });
        return found;
    }

private:
    oracle::Oracle oracle_;
    oracle::FaultyProvider faulty_;
    oracle::RequestLog recorder_;
    FakeClock clock_;
    std::optional<agents::Army> army_;
};

class OutlineStageTest : public ::testing::Test {
protected:
    Result<StageOutput> run(RaiseArmy army, const Source& source = source_with(),
                            const knowledge::Transcript& transcript = lecture()) {
        StageContext context{.job = &job_, .army = std::move(army), .prompts = &prompts_};
        context.inputs.emplace("source", to_json(source));
        context.inputs.emplace("transcript", knowledge::to_json(transcript));
        RR_TRY_ASSIGN(context.fingerprint, stage_.fingerprint(context));
        fingerprint_ = context.fingerprint;
        return stage_.run(context);
    }

    static knowledge::Outline outline_of(const StageOutput& output) {
        return unwrap(knowledge::outline_from_json(output.artifact));
    }

    void use_tier(std::string tier) { job_.tier = std::move(tier); }
    [[nodiscard]] const json::Json& fingerprint() const { return fingerprint_; }

private:
    library::Job job_{.id = "J1", .tier = "max"};
    const prompt::Library prompts_ = unwrap(prompt::Library::embedded());
    const OutlineStage stage_;
    json::Json fingerprint_;
};

// --- Sections from the commander's outlines --------------------------------------

TEST_F(OutlineStageTest, TheCommandersOutlinesCutTheVideoWhereTheTalkTurns) {
    Rig rig;
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, IsEmpty());
    EXPECT_TRUE(output.reusable);
    const knowledge::Outline outline = outline_of(output);
    EXPECT_EQ(outline, (knowledge::Outline{.sections = lecture_sections(),
                                           .brief = lecture_brief(),
                                           .sections_from = SectionsFrom::models}));

    // One reading of the whole, three samples drawn at once, thinking within
    // the tier's budget and ending as the commander ends its thinking.
    const std::vector<llm::ChatRequest> outlines = rig.requests("outline");
    ASSERT_EQ(outlines.size(), 1U);
    EXPECT_EQ(outlines[0].n, 3);
    EXPECT_EQ(outlines[0].thinking, true);
    EXPECT_EQ(outlines[0].thinking_budget, (llm::ThinkingBudget{.tokens = 16'384, .closing = "Answer now."}));
    EXPECT_THAT(outlines[0].messages[0].text(), HasSubstr("\"Weighing radium\", by Science Hour"));
    EXPECT_THAT(outlines[0].messages.back().text(), HasSubstr("The sentences to outline, the whole video:"));
    EXPECT_THAT(outlines[0].messages.back().text(), HasSubstr("Your first section starts at S1"));

    // Each section described without thinking, then the whole.
    const std::vector<llm::ChatRequest> described = rig.requests("describe");
    ASSERT_EQ(described.size(), 4U);
    for (const llm::ChatRequest& request : described) {
        EXPECT_EQ(request.thinking, false) << request.tag.mission_id;
        EXPECT_EQ(request.messages[0].text(), described[0].messages[0].text())
            << "every description shares its system message, for the engine's cache";
    }
    EXPECT_THAT(described[1].messages.back().text(),
                HasSubstr("The section \"Now, the scales we weigh\", from 3:30 to 7:25:"));
    const std::vector<llm::ChatRequest> briefs = rig.requests("brief");
    ASSERT_EQ(briefs.size(), 1U);
    EXPECT_EQ(briefs[0].thinking, false);
    EXPECT_THAT(briefs[0].messages.back().text(),
                HasSubstr("2. [3:30] Now, the scales we weigh: Now, the scales we weigh it with.\n"));
}

TEST_F(OutlineStageTest, ASectionStartsWhereEnoughSamplesAgree) {
    // One engine slot: the three samples are three calls. The first sample
    // starts the third section a sentence late and adds an aside the others
    // do not see.
    Rig rig{{oracle::edit("disagree", oracle::When{.mission = "outline-1", .calls = 1},
                          [](const std::string& answer) {
                              return replace_all(answer, R"({"first":"S16")",
                                                 R"({"first":"S12","title":"An aside"},{"first":"S17")");
                          })},
            engines(32'768, 1)};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_EQ(rig.requests("outline").size(), 3U);
    EXPECT_EQ(outline_of(output).sections, lecture_sections())
        << "S16, where two of three start it and the third is a sentence off; no aside";
}

TEST_F(OutlineStageTest, AFasterTierOutlinesOnceWithASmallerBudget) {
    use_tier("fast");
    Rig rig;
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_EQ(outline_of(output).sections, lecture_sections());
    const std::vector<llm::ChatRequest> outlines = rig.requests("outline");
    ASSERT_EQ(outlines.size(), 1U);
    EXPECT_FALSE(outlines[0].n.has_value()) << "one sample";
    EXPECT_EQ(outlines[0].thinking_budget, (llm::ThinkingBudget{.tokens = 8'192, .closing = "Answer now."}));
}

// --- Sections from chapters ----------------------------------------------------------

TEST_F(OutlineStageTest, TheCreatorsChaptersAreTheSectionsWhenThereAreTwoOrMore) {
    Rig rig;
    const Source source = source_with({chapter("Welcome", 0, 205), chapter("The scales", 205, 445),
                                       chapter("Samples and numbers", 445, 900)});
    const StageOutput output = unwrap(run(rig.raise(), source));
    EXPECT_THAT(output.findings, IsEmpty());
    const knowledge::Outline outline = outline_of(output);
    EXPECT_EQ(outline.sections_from, SectionsFrom::chapters);
    EXPECT_EQ(outline.sections,
              (std::vector<Section>{Section{.first = SentenceId{1},
                                            .last = SentenceId{7},
                                            .title = "Welcome",
                                            .gist = "Welcome back to the shed, everyone."},
                                    Section{.first = SentenceId{8},
                                            .last = SentenceId{15},
                                            .title = "The scales",
                                            .gist = "Now, the scales we weigh it with."},
                                    Section{.first = SentenceId{16},
                                            .last = SentenceId{30},
                                            .title = "Samples and numbers",
                                            .gist = "Next, the samples from the NRC lab."}}))
        << "3:25 is closest to S8's 3:30, 7:25 to S16's 7:30";
    EXPECT_TRUE(rig.requests("outline").empty()) << "the creator said where";
    EXPECT_EQ(rig.requests("describe").size(), 3U);
    EXPECT_EQ(outline.brief.speakers, (std::vector<Speaker>{Speaker{.name = "Ada"}}));

    // One chapter cuts nothing: the commander outlines.
    Rig again;
    EXPECT_EQ(
        outline_of(unwrap(run(again.raise(), source_with({chapter("Everything", 0, 900)})))).sections_from,
        SectionsFrom::models);
}

TEST_F(OutlineStageTest, AChapterLongerThanEightMinutesIsCutWhereTheSpeakerPausedLongest) {
    Rig rig;
    const StageOutput output =
        unwrap(run(rig.raise(), source_with({chapter("All of it", 0, 600), chapter("The end", 600, 900)})));
    std::vector<std::string> titles;
    for (const Section& section : outline_of(output).sections) {
        titles.push_back(std::format("{}\u2013{} {}", section.first, section.last, section.title));
    }
    EXPECT_THAT(titles, ElementsAre("S1\u2013S10 All of it (1 of 2)", "S11\u2013S20 All of it (2 of 2)",
                                    "S21\u2013S30 The end"))
        << "9:55 of talk, every pause as long: cut nearest its middle";
    EXPECT_EQ(rig.requests("describe").size(), 3U) << "each part described on its own";
}

// --- Without a pod -------------------------------------------------------------------

TEST_F(OutlineStageTest, WithoutAPodTheVideoIsCutWhereTheSpeakerPausedAndItsBriefIsEmpty) {
    const StageOutput output = unwrap(run({}));
    EXPECT_EQ(
        outline_of(output),
        (knowledge::Outline{
            .sections = {Section{.first = SentenceId{1}, .last = SentenceId{15}, .title = "0:00\u20137:25"},
                         Section{
                             .first = SentenceId{16}, .last = SentenceId{30}, .title = "7:30\u201314:55"}},
            .sections_from = SectionsFrom::pauses}));
    EXPECT_THAT(
        output.findings,
        ElementsAre("No pod is configured to read the video: its sections are cut where the speaker paused, "
                    "without gists, and its brief is empty. Set up the pod (rreport doctor says how) and "
                    "ingest the video again."));
    EXPECT_TRUE(fingerprint().at("models").is_null());

    const StageOutput chaptered =
        unwrap(run({}, source_with({chapter("Welcome", 0, 205), chapter("Rest", 205, 900)})));
    EXPECT_EQ(outline_of(chaptered).sections_from, SectionsFrom::chapters);
    EXPECT_EQ(outline_of(chaptered).sections.size(), 3U) << "the rest, 11:25 long, cut in two";
    EXPECT_THAT(chaptered.findings,
                ElementsAre(HasSubstr("its sections are the creator's chapters, without gists")));
}

// --- Windows -------------------------------------------------------------------------

TEST_F(OutlineStageTest, ATranscriptTooLongForTheCommanderIsOutlinedInWindows) {
    use_tier("fast");
    Rig whole;
    const knowledge::Outline at_once = outline_of(unwrap(run(whole.raise(), source_with(), long_lecture())));
    const llm::ChatRequest read = whole.requests("outline").front();
    const agents::Footprint footprint = unwrap(agents::footprint_of(read, prompt::TokenEstimator{}));

    // A commander that holds one token less than reading it all at once
    // takes, and twice the tier's budget at least, so that it thinks as long.
    const auto holds = static_cast<int>(footprint.prompt + footprint.answer - 1);
    ASSERT_GE(holds / 2, 8'192);
    Rig windowed{{}, engines(holds)};
    const StageOutput output = unwrap(run(windowed.raise(), source_with(), long_lecture()));
    EXPECT_THAT(output.findings, IsEmpty());
    EXPECT_EQ(outline_of(output), at_once) << "the talk turns where it turns, however it is read";
    EXPECT_EQ(at_once.sections.size(), 26U);

    const std::vector<llm::ChatRequest> outlines = windowed.requests("outline");
    ASSERT_EQ(outlines.size(), 2U);
    EXPECT_THAT(outlines[0].messages.back().text(), HasSubstr("Your first section starts at S1"));
    EXPECT_THAT(outlines[0].messages.back().text(), HasSubstr("The sentences to outline, from 0:00 to "));
    const std::string second = outlines[1].messages.back().text();
    EXPECT_THAT(second, HasSubstr("The sentences before them, for context only"));
    EXPECT_THAT(second, HasSubstr("List only the sections that start among these sentences."));
    EXPECT_THAT(second, HasSubstr("of the video's 2:09:55:"));
}

TEST_F(OutlineStageTest, AnOutlineThinksWithinHalfOfWhatTheCommanderHolds) {
    Rig rig{{}, engines(16'384)};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_EQ(outline_of(output).sections, lecture_sections());
    EXPECT_EQ(rig.requests("outline").front().thinking_budget,
              (llm::ThinkingBudget{.tokens = 8'192, .closing = "Answer now."}))
        << "the tier's 16,384 tokens would leave the transcript too little room";
}

TEST_F(OutlineStageTest, AnEngineThatHoldsNoSentenceLeavesTheVideoCutAtPauses) {
    Rig rig{{}, engines(2'048)};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(
        output.findings,
        ElementsAre(
            "the sentences S1\u2013S30 (0:00 to 14:55) have no outline: even alone, each is more than "
            "qwen-commander holds at once; cut where the speaker paused instead",
            "section 1, \"0:00\u20137:25\" (0:00 to 7:25), has no gist: it is more than qwen-commander "
            "holds at once",
            "section 2, \"7:30\u201314:55\" (7:30 to 14:55), has no gist: it is more than "
            "qwen-commander holds at once",
            "the video has no gist: no section has one to write it from"))
        << "one finding for the stretch, not one a sentence";
    EXPECT_EQ(outline_of(output).sections_from, SectionsFrom::pauses);
    EXPECT_TRUE(rig.requests("outline").empty());
}

TEST_F(OutlineStageTest, ASentenceTooLongForTheCommanderEvenAloneStartsASection) {
    std::vector<std::string> texts = lecture_texts();
    std::string endless = "Sample";
    for (int word = 0; word < 12'000; ++word) {
        endless += " glowed";
    }
    texts[11] = endless + ".";
    use_tier("fast");
    Rig rig{{}, engines(16'384)};
    const StageOutput output = unwrap(run(rig.raise(), source_with(), transcript_of(texts)));
    EXPECT_THAT(output.findings,
                Contains("the sentence S12 (5:30 to 5:55) has no outline: even alone, it is more than "
                         "qwen-commander holds at once; a section starts there"));
    EXPECT_THAT(output.findings,
                Contains(HasSubstr("(5:30 to 7:25), has no gist: it is more than qwen-commander "
                                   "holds at once")));
    std::vector<SentenceId> firsts;
    for (const Section& section : outline_of(output).sections) {
        firsts.push_back(section.first);
    }
    EXPECT_THAT(firsts,
                ElementsAre(SentenceId{1}, SentenceId{8}, SentenceId{12}, SentenceId{16}, SentenceId{24}));
}

// --- Checks, and repairs ---------------------------------------------------------------

// A fault that rewrites `from` into `to` in the first answer of `mission`.
oracle::Fault rewrite(std::string mission, std::string from, std::string to) {
    return oracle::edit("rewrite", oracle::When{.mission = std::move(mission), .calls = 1},
                        [from = std::move(from), to = std::move(to)](const std::string& answer) {
                            return replace_all(answer, from, to);
                        });
}

std::string words(int count) {
    std::string text = "Radium";
    for (int word = 1; word < count; ++word) {
        text += " glows";
    }
    return text;
}

TEST_F(OutlineStageTest, EveryRuleAnAnswerBreaksIsSentBackAndRepaired) {
    std::string every_sentence = R"({"sections":[)";
    for (int number = 1; number <= 30; ++number) {
        every_sentence +=
            std::format(R"({}{{"first":"S{}","title":"Part {}"}})", number == 1 ? "" : ",", number, number);
    }
    every_sentence += "]}";
    std::string many_terms;
    for (int term = 1; term <= 16; ++term) {
        many_terms += std::format(R"(,{{"meaning":"{}","term":"NRC"}})", term);
    }
    struct Case {
        oracle::Fault fault;
        std::string kind;
        std::string sent_back;
    };
    std::vector<Case> cases;
    cases.push_back({rewrite("outline-1", R"({"first":"S1","title":"Welcome back to the shed"},)", ""),
                     "outline", "the first section starts at S1, where the video starts"});
    cases.push_back(
        {rewrite("outline-1", R"("first":"S16")", R"("first":"S5")"), "outline",
         "section 3 starts at S5, not after section 2 at S8: list the sections in the order said, "
         "each once"});
    cases.push_back(
        {rewrite("outline-1", R"("first":"S24")", R"("first":"S31")"), "outline",
         "section 4 starts at S31: list only the sections that start among the sentences to outline, "
         "S1\u2013S30"});
    cases.push_back({rewrite("outline-1", R"("title":"Now, the scales we weigh")",
                             std::format(R"("title":"{}")", words(14))),
                     "outline", "section 2 has a title of 14 words: name it in 12 at most"});
    cases.push_back(
        {oracle::edit("crowd", oracle::When{.mission = "outline-1", .calls = 1},
                      [every_sentence](const std::string&) { return every_sentence; }),
         "outline",
         "30 sections in 15 minutes of video: a section is minutes of talk about one thing, not a "
         "sentence or two; join the sections about the same thing"});
    cases.push_back(
        {rewrite("describe-1", R"("name":"Ada")", R"("name":"Pierre")"), "describe",
         "speaker \"Pierre\" is named neither in the section nor in the video's description: list only "
         "who the video names"});
    cases.push_back({rewrite("describe-3", R"("term":"NRC")", R"("term":"IAEA")"), "describe",
                     "term \"IAEA\" is not said in the section: list only its own words, as they are said"});
    cases.push_back({rewrite("describe-3", R"("term":"NRC"})", std::string{R"("term":"NRC"})"} + many_terms),
                     "describe", "17 terms: list at most 15, those a reader would stumble on"});
    cases.push_back(
        {rewrite("describe-2", R"("gist":"Now, the scales we weigh it with.")",
                 std::format(R"("gist":"{}")", words(61))),
         "describe",
         "the gist has 61 words: say what the section says in one or two sentences, 60 words at most"});
    cases.push_back({rewrite("brief", R"("gist":"A video)", std::format(R"("gist":"{} A video)", words(140))),
                     "brief", "the gist has 157 words: tell the video in one paragraph, 150 words at most"});
    for (Case& test : cases) {
        SCOPED_TRACE(test.sent_back);
        Rig rig{{std::move(test.fault)}};
        const StageOutput output = unwrap(run(rig.raise()));
        EXPECT_THAT(output.findings, IsEmpty());
        EXPECT_EQ(outline_of(output), (knowledge::Outline{.sections = lecture_sections(),
                                                          .brief = lecture_brief(),
                                                          .sections_from = SectionsFrom::models}))
            << "repaired";
        std::vector<std::string> last_messages;
        for (const llm::ChatRequest& request : rig.requests(test.kind)) {
            last_messages.push_back(request.messages.back().text());
        }
        EXPECT_THAT(last_messages, Contains(HasSubstr(test.sent_back)));
    }
}

TEST_F(OutlineStageTest, ASpeakerTheVideosDescriptionNamesIsTaken) {
    Source source = source_with();
    source.video.description = "With Ada Lovelace, from the shed.";
    Rig rig{{rewrite("describe-2", R"("speakers":[])",
                     R"("speakers":[{"name":"Ada Lovelace","role":"lecturer"}])")}};
    const StageOutput output = unwrap(run(rig.raise(), source));
    EXPECT_THAT(output.findings, IsEmpty());
    EXPECT_EQ(
        outline_of(output).brief.speakers,
        (std::vector<Speaker>{Speaker{.name = "Ada"}, Speaker{.name = "Ada Lovelace", .role = "lecturer"}}))
        << "named by the description, though not by the section";
    EXPECT_EQ(rig.requests("describe").size(), 4U) << "taken at once, not sent back";
}

// --- What no model could do --------------------------------------------------------------

TEST_F(OutlineStageTest, AStretchTheModelsDeclineToOutlineIsCutWhereTheSpeakerPaused) {
    Rig rig{{oracle::refusal(oracle::When{.kind = "outline", .calls = 0})}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings,
                ElementsAre("the sentences S1\u2013S30 (0:00 to 14:55) have no outline: the models "
                            "declined to outline them; cut where the speaker paused instead"));
    EXPECT_TRUE(output.reusable) << "they would decline again";
    const knowledge::Outline outline = outline_of(output);
    EXPECT_EQ(outline.sections_from, SectionsFrom::pauses);
    ASSERT_EQ(outline.sections.size(), 2U);
    EXPECT_EQ(outline.sections[1].title, "7:30\u201314:55");
    EXPECT_EQ(outline.sections[1].gist, "Next, the samples from the NRC lab.") << "described all the same";
}

TEST_F(OutlineStageTest, ASectionTheModelsDeclineToDescribeHasNoGistAndTheBriefGoesOn) {
    Rig rig{{oracle::refusal(oracle::When{.mission = "describe-1", .calls = 0})}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings,
                ElementsAre("section 1, \"Welcome back to the shed\" (0:00 to 3:25), has no gist: the models "
                            "declined to describe it"));
    const knowledge::Outline outline = outline_of(output);
    EXPECT_FALSE(outline.sections[0].gist.has_value());
    EXPECT_EQ(outline.sections[1].gist, "Now, the scales we weigh it with.");
    EXPECT_THAT(outline.brief.speakers, IsEmpty()) << "Ada spoke in the section no one described";
    EXPECT_EQ(outline.brief.lexicon, (std::vector<LexiconTerm>{LexiconTerm{.term = "NRC"}}));
    EXPECT_THAT(rig.requests("brief").front().messages.back().text(),
                HasSubstr("1. [0:00] Welcome back to the shed\n2. [3:30]"))
        << "a section without a gist is listed by its title";
}

TEST_F(OutlineStageTest, AVideoGistTheModelsDeclineToWriteIsAFinding) {
    Rig rig{{oracle::refusal(oracle::When{.kind = "brief", .calls = 0})}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, ElementsAre("the video has no gist: the models declined to write it"));
    knowledge::Brief brief = lecture_brief();
    brief.gist.reset();
    EXPECT_EQ(outline_of(output).brief, brief);
}

TEST_F(OutlineStageTest, AJobCeilingMakesTheResultThisJobsAlone) {
    Rig rig{{}, engines(), agents::Ceilings{.completion_tokens = 1}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_FALSE(output.reusable) << "a job allowed more would have described them";
    EXPECT_THAT(
        output.findings,
        Contains("section 2, \"Now, the scales we weigh\" (3:30 to 7:25), has no gist: the job reached its "
                 "completion_tokens ceiling first"));
    EXPECT_THAT(output.findings, Contains("the video has no gist: no section has one to write it from"));
    EXPECT_EQ(outline_of(output).sections.size(), 4U) << "the outline came first, and stands";
}

// --- What is refused, and the key ----------------------------------------------------------

TEST_F(OutlineStageTest, ATranscriptWithoutSentencesIsNothingToOutline) {
    Rig rig;
    EXPECT_ERRC(run(rig.raise(), source_with(), knowledge::Transcript{}), Errc::invalid_argument);
}

TEST_F(OutlineStageTest, ATierRReportDoesNotShipIsNotFound) {
    use_tier("turbo");
    EXPECT_ERRC(run({}), Errc::not_found);
}

TEST_F(OutlineStageTest, TheKeyHoldsTheTierThePromptsTheArmyAndWhatTheCommanderHolds) {
    Rig rig;
    unwrap(run(rig.raise()));
    const json::Json max = fingerprint();
    EXPECT_EQ(max.at("longest_ms"), 480'000);
    const json::Json& models = max.at("models");
    EXPECT_EQ(models.at("prompts").at("outline").at("label"), "outline@1");
    EXPECT_EQ(models.at("prompts").at("describe").at("label"), "describe@1");
    EXPECT_EQ(models.at("prompts").at("brief").at("label"), "brief@1");
    EXPECT_EQ(models.at("prompts").at("outline").at("hash"),
              unwrap(unwrap(prompt::Library::embedded()).content_hash("outline")));
    EXPECT_EQ(models.at("holds"), 32'768);
    EXPECT_EQ(models.at("context"), (json::Json{{"sentences", 3}, {"bytes", 1'000}}));

    use_tier("fast");
    Rig fast;
    unwrap(run(fast.raise()));
    EXPECT_NE(fingerprint().at("tier"), max.at("tier"));
    EXPECT_EQ(fingerprint().at("models"), max.at("models"));
}

}  // namespace
}  // namespace rr::pipeline

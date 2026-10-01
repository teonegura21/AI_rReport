// The transcript stage on synthetic captions: punctuated ones as they are, and
// unpunctuated ones punctuated by an army whose models are the oracle, with
// the faults real models make injected where a test says.
#include <rr/pipeline/transcript_stage.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/core/strings.hpp>
#include <rr/knowledge/transcript.hpp>
#include <rr/oracle/answers.hpp>
#include <rr/oracle/oracle.hpp>
#include <rr/pipeline/source.hpp>
#include <rr/testkit/result.hpp>

namespace rr::pipeline {
namespace {

using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::Not;
using testkit::unwrap;

// The numbers of the script's samples.
constexpr std::array<std::string_view, 30> kNumbers{
    "one",         "two",        "three",        "four",         "five",         "six",
    "seven",       "eight",      "nine",         "ten",          "eleven",       "twelve",
    "thirteen",    "fourteen",   "fifteen",      "sixteen",      "seventeen",    "eighteen",
    "nineteen",    "twenty",     "twenty-one",   "twenty-two",   "twenty-three", "twenty-four",
    "twenty-five", "twenty-six", "twenty-seven", "twenty-eight", "twenty-nine",  "thirty"};

// The script a synthetic lecture was read from: thirty sentences, with names
// for the capitals to find.
std::vector<std::string> script_sentences() {
    std::vector<std::string> sentences;
    sentences.reserve(kNumbers.size());
    for (std::size_t index = 0; index < kNumbers.size(); ++index) {
        sentences.push_back(
            index % 3 == 2 ? std::format("Did Marie Curie weigh sample {} again in Paris?", kNumbers[index])
                           : std::format("In the shed, Pierre Curie heated sample {} until it glowed, and "
                                         "wrote the result down.",
                                         kNumbers[index]));
    }
    return sentences;
}

std::string script() {
    std::string text;
    for (const std::string& sentence : script_sentences()) {
        text += text.empty() ? sentence : " " + sentence;
    }
    return text;
}

// The script as automatic captions write it: lowercase, without punctuation,
// in WebVTT cues of eight words, a word every 300 ms.
std::string automatic_captions(std::string_view text) {
    std::vector<std::string> words;
    for (std::size_t begin = 0; begin < text.size();) {
        const std::size_t end = std::min(text.find(' ', begin), text.size());
        std::string word;
        for (const char character : to_lower_ascii(text.substr(begin, end - begin))) {
            if (character != ',' && character != '.' && character != '?') {
                word += character;
            }
        }
        words.push_back(word);
        begin = end + 1;
    }
    std::string vtt = "WEBVTT\n";
    const auto at = [](std::size_t index) {
        return format_timestamp(Millis{static_cast<std::int64_t>(index) * 300});
    };
    for (std::size_t first = 0; first < words.size(); first += 8) {
        const std::size_t last = std::min(first + 8, words.size());
        vtt += std::format("\n{}.000 --> {}.000\n", at(first), at(last));
        for (std::size_t index = first; index < last; ++index) {
            vtt += index == first ? words[index] : " " + words[index];
        }
        vtt += '\n';
    }
    return vtt;
}

json::Json source_with(std::string captions,
                       transcript::CaptionFormat format = transcript::CaptionFormat::vtt) {
    // A description longer than the prompt shows: about 1,900 bytes.
    std::string description = "On radium and polonium.\n";
    for (int line = 1; line <= 40; ++line) {
        description += std::format("Part {}: how the Curies measured what they could not see.\n", line);
    }
    description += "THE END";
    return to_json(Source{.origin = Origin::web,
                          .video = media::Video{.id = "abc123XYZ_0",
                                                .title = "On radium",
                                                .description = description,
                                                .channel = "Science Hour"},
                          .captions = SourceCaptions{.language = "en",
                                                     .kind = media::CaptionKind::automatic,
                                                     .format = format,
                                                     .content = std::move(captions)}});
}

std::vector<agents::Engine> engines() {
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
                           .slots = 2,
                           .context = 16384,
                           .ctx_per_slot = 8192,
                           .direct = direct,
                           .thinking = thinking},
            agents::Engine{.model = "gemma-infantry",
                           .checksum = "sha-g",
                           .roles = {"infantry"},
                           .family = "gemma",
                           .slots = 4,
                           .context = 16384,
                           .ctx_per_slot = 4096,
                           .direct = direct,
                           .thinking = thinking}};
}

// An army whose models are the oracle, punctuating from the script, with
// `faults` made where they say.
struct Rig {
    explicit Rig(std::vector<oracle::Fault> faults = {}, agents::Ceilings ceilings = {})
        : faulty(oracle, std::move(faults))
        , recorder(faulty) {
        oracle::teach_stages(oracle, script());
        army.emplace(unwrap(agents::Army::create(
            agents::Army::Options{.engines = engines(), .job_id = "J1", .ceilings = ceilings}, recorder,
            clock)));
    }

    RaiseArmy raise() {
        return [this]() -> Result<agents::Army*> {
            ++raised;
            return &*army;
        };
    }

    oracle::Oracle oracle;
    oracle::FaultyProvider faulty;
    oracle::RequestLog recorder;
    FakeClock clock;
    std::optional<agents::Army> army;
    int raised = 0;
};

class TranscriptStageTest : public ::testing::Test {
protected:
    Result<StageOutput> run(json::Json source, RaiseArmy army = {}) {
        StageContext context{.job = &job_, .army = std::move(army), .prompts = &prompts_};
        context.inputs.emplace("source", std::move(source));
        RR_TRY_ASSIGN(context.fingerprint, stage_.fingerprint(context));
        fingerprint_ = context.fingerprint;
        return stage_.run(context);
    }

    static std::vector<std::string> texts(const json::Json& artifact) {
        std::vector<std::string> sentences;
        for (const knowledge::Sentence& sentence :
             unwrap(knowledge::transcript_from_json(artifact)).sentences) {
            sentences.push_back(sentence.text);
        }
        return sentences;
    }

    [[nodiscard]] json::Json& fingerprint() { return fingerprint_; }

private:
    library::Job job_{.id = "J1"};
    const prompt::Library prompts_ = unwrap(prompt::Library::embedded());
    // Windows of about 250 bytes: the script's 30 sentences make about ten,
    // more than the infantry's four slots hold at once.
    const TranscriptStage stage_{WindowSize{.least = 150, .target = 250, .most = 350, .context = 60}};
    json::Json fingerprint_;
};

TEST_F(TranscriptStageTest, PunctuatedCaptionsAreCutAsTheyAreAndNoPodStarts) {
    Rig rig;
    const StageOutput output = unwrap(
        run(source_with(
                "WEBVTT\n\n00:00.000 --> 00:03.000\nRadium glows in the dark.\n\n00:03.000 --> 00:05.000\n"
                "Pierre heated it.\n"),
            rig.raise()));
    EXPECT_EQ(fingerprint(), (json::Json{{"punctuation", "captions"}}));
    EXPECT_EQ(rig.raised, 0) << "the pod is never started for captions that need no model";
    EXPECT_EQ(texts(output.artifact),
              (std::vector<std::string>{"Radium glows in the dark.", "Pierre heated it."}));
    EXPECT_THAT(output.findings, IsEmpty());
}

TEST_F(TranscriptStageTest, WithoutAPodUnpunctuatedCaptionsAreCutWhereTheSpeakerPaused) {
    const StageOutput output = unwrap(run(source_with(automatic_captions(script()))));
    EXPECT_EQ(fingerprint(), (json::Json{{"punctuation", "none"}}));
    EXPECT_THAT(output.findings, ElementsAre(HasSubstr("no pod is configured")));
    for (const std::string& sentence : texts(output.artifact)) {
        EXPECT_LE(std::ranges::count(sentence, ' ') + 1, 60) << "60 words at most";
    }
}

TEST_F(TranscriptStageTest, TheInfantryPunctuatesThemWithoutChangingAWord) {
    Rig rig;
    const StageOutput output = unwrap(run(source_with(automatic_captions(script())), rig.raise()));
    EXPECT_EQ(fingerprint().at("punctuation"), "models");
    EXPECT_EQ(fingerprint().at("prompt"), "punctuate@1");
    EXPECT_EQ(fingerprint().at("army").size(), 2U) << "its key names the models that answer";
    EXPECT_EQ(texts(output.artifact), script_sentences()) << "the script's sentences, from its captions";
    EXPECT_THAT(output.findings, IsEmpty());
    EXPECT_TRUE(output.reusable);

    const std::vector<llm::ChatRequest> requests = rig.recorder.requests();
    ASSERT_GE(requests.size(), 3U) << "several windows";
    for (const llm::ChatRequest& request : requests) {
        EXPECT_EQ(request.model, "gemma-infantry") << "the infantry's work";
        EXPECT_EQ(request.temperature, 0.0) << "the most likely punctuation, every time";
        EXPECT_EQ(request.messages.front().text(), requests.front().messages.front().text())
            << "every window shares the system message, which the engine's cache keeps";
    }
    const std::string system = requests.front().messages.front().text();
    EXPECT_THAT(system, HasSubstr("the video \"On radium\", by Science Hour.\nIts description shows"));
    EXPECT_THAT(system, HasSubstr("<description>\nOn radium and polonium.\nPart 1: how the Curies"));
    EXPECT_THAT(system, HasSubstr("could not see.\n\u2026\n</description>"))
        << "a long description is cut after a line";
    EXPECT_THAT(system, Not(HasSubstr("THE END")));
}

TEST_F(TranscriptStageTest, AWindowNoModelPunctuatesKeepsTheCaptionsFormAndSaysWhere) {
    // Every answer to the second window, at every rung, drops its full stops.
    Rig rig{{oracle::edit(
        "unpunctuated", oracle::When{.mission = "punctuate-2", .calls = 0},
        [](const std::string& answer) { return replace_all(replace_all(answer, ".", ""), "?", ""); })}};
    const StageOutput output = unwrap(run(source_with(automatic_captions(script())), rig.raise()));
    ASSERT_EQ(output.findings.size(), 1U);
    EXPECT_THAT(output.findings[0], HasSubstr("the words said from"));
    EXPECT_THAT(output.findings[0],
                HasSubstr("keep the captions' form, unpunctuated: no answer passed its checks"));
    EXPECT_THAT(output.findings[0], HasSubstr("Mark where each sentence ends"));
    EXPECT_TRUE(output.reusable) << "the models could not, and would not next time either";
    const std::vector<std::string> sentences = texts(output.artifact);
    EXPECT_EQ(sentences.front(), script_sentences().front()) << "the other windows are punctuated";
    EXPECT_EQ(sentences.back(), script_sentences().back());
    EXPECT_THAT(sentences, Not(Contains(script_sentences()[3]))) << "the second window's words are not";
    EXPECT_LT(sentences.size(), script_sentences().size());
}

TEST_F(TranscriptStageTest, AWindowTheModelsDeclineIsAFinding) {
    Rig rig{{oracle::refusal(oracle::When{.mission = "punctuate-1", .calls = 0})}};
    const StageOutput output = unwrap(run(source_with(automatic_captions(script())), rig.raise()));
    EXPECT_THAT(output.findings, ElementsAre(HasSubstr("the models declined to punctuate them")));
}

TEST_F(TranscriptStageTest, AWindowAJobCeilingStoppedMakesTheResultThisJobsAlone) {
    Rig rig{{}, agents::Ceilings{.completion_tokens = 1}};
    const StageOutput output = unwrap(run(source_with(automatic_captions(script())), rig.raise()));
    EXPECT_FALSE(output.reusable) << "a job allowed more would have punctuated it";
    EXPECT_THAT(output.findings, Contains(HasSubstr("the job reached its completion_tokens ceiling first")));
}

TEST_F(TranscriptStageTest, CaptionsWithoutWordsAreRefused) {
    EXPECT_ERRC(run(source_with("WEBVTT\n\n00:00.000 --> 00:03.000\n[Music]\n")), Errc::not_found);
}

}  // namespace
}  // namespace rr::pipeline

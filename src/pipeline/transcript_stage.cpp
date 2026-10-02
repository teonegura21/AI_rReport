// The second stage: captions into numbered sentences (rr/pipeline/transcript_stage.hpp).
#include <rr/pipeline/transcript_stage.hpp>

#include <cstdint>
#include <format>
#include <memory>
#include <utility>

#include <rr/agents/mission.hpp>
#include <rr/core/assert.hpp>
#include <rr/core/time_span.hpp>
#include <rr/knowledge/transcript.hpp>
#include <rr/llm/memo.hpp>
#include <rr/pipeline/asking.hpp>
#include <rr/pipeline/source.hpp>
#include <rr/prompt/tokens.hpp>
#include <rr/transcript/sentences.hpp>

namespace rr::pipeline {

namespace {

using transcript::Word;

constexpr std::string_view kNoPod =
    "The captions are not punctuated, and no pod is configured to punctuate them: sentences were cut where "
    "the speaker paused, 60 words at most. Set up the pod (rreport doctor says how) and ingest the video "
    "again.";

struct Read {
    Source source{};
    transcript::CaptionTrack track{};
};

Result<Read> read_input(const StageContext& context) {
    RR_TRY_ASSIGN(Source source, source_from_json(context.inputs.at(std::string{SourceStage::kName})));
    RR_TRY_ASSIGN(transcript::CaptionTrack track,
                  transcript::read_captions(source.captions.content, source.captions.format));
    if (track.words.empty()) {
        return fail(Errc::not_found,
                    "the captions hold no words: nothing was said, or nothing was captioned");
    }
    return Read{.source = std::move(source), .track = std::move(track)};
}

// "the words said from 12:31 to 13:05"
std::string said(const std::vector<Word>& words, const Window& window) {
    return std::format("the words said from {} to {}", format_timestamp(words[window.begin].start),
                       format_timestamp(words[window.end - 1].end));
}

// The window's mission: punctuate its excerpt, keeping every word.
Result<agents::Mission> mission_for(const std::vector<Word>& words, const Window& window, std::size_t index,
                                    const prompt::Library& prompts, const prompt::PromptInfo& info,
                                    const json::Json& shared,
                                    const std::shared_ptr<const json::Schema>& schema) {
    const std::string excerpt = excerpt_of(words, window);
    RR_TRY_ASSIGN(const prompt::Rendered rendered,
                  prompts.render(info.name, shared, json::Json{{"excerpt", excerpt}}));
    const std::string id = std::format("punctuate-{}", index + 1);
    // The answer is the excerpt again with its punctuation: a quarter more
    // than the excerpt, as the default estimator (which errs high) counts it.
    const std::int64_t answer_tokens = (prompt::TokenEstimator{}.estimate(excerpt) * 5 / 4) + 32;
    return agents::Mission{
        .id = id,
        .kind = std::string{TranscriptStage::kPrompt},
        .prompt = info.label(),
        .role = "infantry",
        // Punctuation is not a matter of taste: the most likely answer, the
        // same every time. The salt stays out of the seed for the same reason.
        .request = llm::ChatRequest{.messages = {llm::Message::system(rendered.system),
                                                 llm::Message::user(rendered.user)},
                                    .temperature = 0.0,
                                    .seed = llm::derived_seed(0, TranscriptStage::kName, id, 0)},
        .answer_tokens = static_cast<int>(answer_tokens),
        .schema = schema,
        .check = [&words, window](const json::Json& answer) {
            return punctuation_problems(words, window, answer.at("text").get<std::string>());
        }};
}

struct Punctuated {
    std::vector<Word> words{};
    std::vector<std::string> findings{};
    bool reusable = true;
};

Result<Punctuated> punctuate(const std::vector<Word>& words, const Source& source, agents::Army& army,
                             const StageContext& context, const WindowSize& size) {
    RR_ASSERT(context.prompts != nullptr, "a stage that asks models is given the prompts");
    const prompt::PromptInfo* info = context.prompts->find(TranscriptStage::kPrompt);
    if (info == nullptr) {
        return fail(Errc::not_found, "the prompts have no 'punctuate'");
    }
    RR_TRY_ASSIGN(const std::shared_ptr<const json::Schema> schema,
                  context.prompts->answer_schema(TranscriptStage::kPrompt));
    const std::vector<Window> windows = windows_of(words, size);
    const json::Json shared = video_shared(source.video);
    std::vector<agents::Mission> missions;
    for (std::size_t index = 0; index < windows.size(); ++index) {
        RR_TRY_ASSIGN(agents::Mission mission,
                      mission_for(words, windows[index], index, *context.prompts, *info, shared, schema));
        missions.push_back(std::move(mission));
    }
    if (context.progress) {
        context.progress(
            std::format("transcript: punctuating {} words in {} windows", words.size(), windows.size()));
    }
    RR_TRY_ASSIGN(const std::vector<agents::Outcome> outcomes, army.squad(std::move(missions), context.stop));

    Punctuated done{.words = words};
    for (std::size_t index = 0; index < windows.size(); ++index) {
        const Window& window = windows[index];
        const agents::Outcome& outcome = outcomes[index];
        if (outcome.verdict == agents::Verdict::accepted) {
            RR_TRY_ASSIGN(
                std::vector<Word> own,
                punctuated_words(words, window, outcome.answers.front().value.at("text").get<std::string>()));
            std::ranges::move(own, done.words.begin() + static_cast<std::ptrdiff_t>(window.begin));
            continue;
        }
        done.findings.push_back(std::format("{} keep the captions' form, unpunctuated: {}.",
                                            said(words, window),
                                            why_unanswered(outcome, army, "punctuate them")));
        if (outcome.verdict == agents::Verdict::ceiling) {
            done.reusable = false;  // a job allowed more would have punctuated them
        }
    }
    return done;
}

}  // namespace

Result<json::Json> TranscriptStage::fingerprint(const StageContext& context) const {
    RR_TRY_ASSIGN(const Read read, read_input(context));
    if (transcript::punctuated(read.track.words)) {
        return json::Json{{"punctuation", "captions"}};
    }
    if (!context.army) {
        return json::Json{{"punctuation", "none"}};
    }
    RR_ASSERT(context.prompts != nullptr, "a stage that asks models is given the prompts");
    const prompt::PromptInfo* info = context.prompts->find(kPrompt);
    if (info == nullptr) {
        return fail(Errc::not_found, "the prompts have no 'punctuate'");
    }
    RR_TRY_ASSIGN(const std::string prompt_hash, context.prompts->content_hash(kPrompt));
    RR_TRY_ASSIGN(agents::Army* const army, context.army());
    return json::Json{{"punctuation", "models"},
                      {"prompt", info->label()},
                      {"prompt_hash", prompt_hash},
                      {"window",
                       {{"least", window_.least},
                        {"target", window_.target},
                        {"most", window_.most},
                        {"context", window_.context}}},
                      {"army", army_fingerprint(*army)}};
}

Result<StageOutput> TranscriptStage::run(const StageContext& context) const {
    RR_TRY_ASSIGN(const Read read, read_input(context));
    const std::string how = context.fingerprint.at("punctuation").get<std::string>();
    if (how == "captions") {
        return StageOutput{.artifact = knowledge::to_json(transcript::transcript_of(read.track.words))};
    }
    if (how == "none") {
        return StageOutput{.artifact = knowledge::to_json(transcript::transcript_of(read.track.words)),
                           .findings = {std::string{kNoPod}}};
    }
    RR_TRY_ASSIGN(agents::Army* const army, context.army());
    RR_TRY_ASSIGN(Punctuated done, punctuate(read.track.words, read.source, *army, context, window_));
    return StageOutput{.artifact = knowledge::to_json(transcript::transcript_of(done.words)),
                       .findings = std::move(done.findings),
                       .reusable = done.reusable};
}

}  // namespace rr::pipeline

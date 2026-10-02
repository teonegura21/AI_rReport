// The stage that cuts a video into sections and writes its brief
// (rr/pipeline/outline_stage.hpp).
#include <rr/pipeline/outline_stage.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <utility>

#include <rr/agents/mission.hpp>
#include <rr/core/assert.hpp>
#include <rr/core/time_span.hpp>
#include <rr/knowledge/outline.hpp>
#include <rr/knowledge/transcript.hpp>
#include <rr/llm/memo.hpp>
#include <rr/pipeline/asking.hpp>
#include <rr/pipeline/outline.hpp>
#include <rr/pipeline/source.hpp>
#include <rr/pipeline/transcript_stage.hpp>
#include <rr/pipeline/windows.hpp>
#include <rr/reason/tier.hpp>
#include <rr/text/align.hpp>
#include <rr/text/tokens.hpp>

namespace rr::pipeline {

namespace {

using knowledge::SentenceId;
using knowledge::Transcript;

constexpr std::string_view kOutline = "outline";
constexpr std::string_view kDescribe = "describe";
constexpr std::string_view kBrief = "brief";

constexpr std::size_t kLeastChapters = 2;     // one chapter cuts nothing
constexpr std::size_t kTitleWords = 12;       // a section's title
constexpr std::size_t kSectionTerms = 15;     // the words a section's description lists
constexpr std::size_t kGistWords = 60;        // a section's gist: one or two sentences
constexpr std::size_t kVideoGistWords = 150;  // the video's: a paragraph
constexpr int kOutlineAnswerTokens = 1'024;   // fifty sections and their titles
constexpr int kDescribeAnswerTokens = 1'024;
constexpr int kBriefAnswerTokens = 1'024;
// A window as large as the whole transcript: plan_windows() then gives up
// sentences until the commander holds it.
constexpr std::size_t kWholeTranscript = std::numeric_limits<std::size_t>::max();

constexpr std::string_view kNoPod =
    "No pod is configured to read the video: its sections are {}, without gists, and its brief is empty. Set "
    "up the pod (rreport doctor says how) and ingest the video again.";

std::size_t words_in(const json::Json& text) {
    return text::terms(text.get<std::string>()).size();
}

// Whether `text` says `words`, word for word (case, accents and punctuation aside).
bool says(std::string_view text, std::string_view words) {
    return text::align(words, text).verbatim();
}

// The schemas allow S1, S2, … only.
SentenceId sentence_of(const json::Json& written) {
    const std::optional<SentenceId> id = SentenceId::parse(written.get<std::string>());
    RR_ASSERT(id.has_value(), "the answer's schema allows S1, S2, \u2026 only");
    return *id;
}

// What the sentences [begin, end) say, joined.
std::string said_by(const Transcript& transcript, std::size_t begin, std::size_t end) {
    std::string said;
    for (std::size_t index = begin; index < end; ++index) {
        said += said.empty() ? transcript.sentences[index].text : " " + transcript.sentences[index].text;
    }
    return said;
}

// "the whole video", or "from 31:05 to 58:40 of the video's 1:02:13".
std::string part_of(const Transcript& transcript, const SentenceWindow& window) {
    if (window.begin == 0 && window.end == transcript.sentences.size()) {
        return "the whole video";
    }
    return std::format("from {} to {} of the video's {}",
                       format_timestamp(transcript.sentences[window.begin].span.start()),
                       format_timestamp(transcript.sentences[window.end - 1].span.end()),
                       format_timestamp(transcript.sentences.back().span.end()));
}

// --- Checks -------------------------------------------------------------------------

// What is wrong with `answer` as an outline of `window` (the header comment).
std::vector<std::string> outline_problems_in(const json::Json& answer, const Transcript& transcript,
                                             const SentenceWindow& window) {
    Problems problems;
    const json::Json& sections = answer.at("sections");
    std::uint32_t previous = 0;  // where the section before starts; 0: none
    std::size_t number = 0;
    for (const json::Json& section : sections) {
        ++number;
        const SentenceId first = sentence_of(section.at("first"));
        const std::size_t index = first.number() - 1;
        if (index < window.begin || index >= window.end) {
            problems.note(
                std::format("section {} starts at {}: list only the sections that start among the "
                            "sentences to outline, {}",
                            number, first, sentence_range(window.begin, window.end)));
        } else if (first.number() <= previous) {
            problems.note(
                std::format("section {} starts at {}, not after section {} at S{}: list the sections "
                            "in the order said, each once",
                            number, first, number - 1, previous));
        }
        previous = std::max(previous, first.number());
        if (const std::size_t words = words_in(section.at("title")); words > kTitleWords) {
            problems.note(std::format("section {} has a title of {} words: name it in {} at most", number,
                                      words, kTitleWords));
        }
    }
    const bool opens = !sections.empty() && sentence_of(sections.front().at("first")).number() == 1;
    if (window.begin == 0 && !opens) {
        problems.note("the first section starts at S1, where the video starts");
    }
    const Millis length =
        transcript.sentences[window.end - 1].span.end() - transcript.sentences[window.begin].span.start();
    const auto minutes =
        static_cast<std::size_t>(std::max<std::int64_t>(1, (length.count() + 59'999) / 60'000));
    if (sections.size() > minutes + 1) {
        problems.note(std::format(
            "{} sections in {} of video: a section is minutes of talk about one thing, not "
            "a sentence or two; join the sections about the same thing",
            sections.size(), minutes == 1 ? std::string{"a minute"} : std::format("{} minutes", minutes)));
    }
    return std::move(problems).list();
}

// What is wrong with `answer` as a description of a section that says
// `said`; `video` is what the prompt showed of the video, which may name a
// speaker too.
std::vector<std::string> description_problems_in(const json::Json& answer, std::string_view said,
                                                 std::string_view video) {
    Problems problems;
    for (const json::Json& speaker : answer.at("speakers")) {
        const std::string name = speaker.at("name").get<std::string>();
        if (!says(said, name) && !says(video, name)) {
            problems.note(
                std::format("speaker \"{}\" is named neither in the section nor in the video's "
                            "description: list only who the video names",
                            name));
        }
    }
    const json::Json& lexicon = answer.at("lexicon");
    for (const json::Json& term : lexicon) {
        if (const std::string written = term.at("term").get<std::string>(); !says(said, written)) {
            problems.note(
                std::format("term \"{}\" is not said in the section: list only its own words, as they "
                            "are said",
                            written));
        }
    }
    if (lexicon.size() > kSectionTerms) {
        problems.note(std::format("{} terms: list at most {}, those a reader would stumble on",
                                  lexicon.size(), kSectionTerms));
    }
    if (const std::size_t words = words_in(answer.at("gist")); words > kGistWords) {
        problems.note(
            std::format("the gist has {} words: say what the section says in one or two sentences, {} "
                        "words at most",
                        words, kGistWords));
    }
    return std::move(problems).list();
}

std::vector<std::string> video_gist_problems_in(const json::Json& answer) {
    if (const std::size_t words = words_in(answer.at("gist")); words > kVideoGistWords) {
        return {std::format("the gist has {} words: tell the video in one paragraph, {} words at most", words,
                            kVideoGistWords)};
    }
    return {};
}

// --- Answers, read ----------------------------------------------------------------

std::vector<Cut> cuts_of(const json::Json& answer) {
    std::vector<Cut> cuts;
    cuts.reserve(answer.at("sections").size());
    for (const json::Json& section : answer.at("sections")) {
        cuts.push_back(Cut{.first = sentence_of(section.at("first")).number() - std::size_t{1},
                           .title = section.at("title").get<std::string>()});
    }
    return cuts;
}

std::optional<std::string> optional_text(const json::Json& value) {
    return value.is_null() ? std::nullopt : std::optional<std::string>{value.get<std::string>()};
}

// A section's description, read (schemas/missions/describe.json).
struct Description {
    std::string gist{};
    knowledge::Brief brief{};  // its share of the video's: genre, speakers, words
};

Description description_of(const json::Json& answer) {
    const std::optional<knowledge::Genre> genre =
        knowledge::genre_named(answer.at("genre").get<std::string>());
    RR_ASSERT(genre.has_value(), "the answer's schema names the artifact's genres only");
    Description description{.gist = answer.at("gist").get<std::string>(), .brief = {.genre = *genre}};
    for (const json::Json& speaker : answer.at("speakers")) {
        description.brief.speakers.push_back(knowledge::Speaker{.name = speaker.at("name").get<std::string>(),
                                                                .role = optional_text(speaker.at("role"))});
    }
    for (const json::Json& term : answer.at("lexicon")) {
        description.brief.lexicon.push_back(knowledge::LexiconTerm{
            .term = term.at("term").get<std::string>(), .meaning = optional_text(term.at("meaning"))});
    }
    return description;
}

// --- The models' part ----------------------------------------------------------------

struct Prompts {
    StagePrompt outline{};
    StagePrompt describe{};
    StagePrompt brief{};
};

Result<Prompts> prompts_of(const prompt::Library& library) {
    Prompts prompts;
    RR_TRY_ASSIGN(prompts.outline, stage_prompt(library, kOutline));
    RR_TRY_ASSIGN(prompts.describe, stage_prompt(library, kDescribe));
    RR_TRY_ASSIGN(prompts.brief, stage_prompt(library, kBrief));
    return prompts;
}

// Where the commander's outlines put the sections.
struct Outlined {
    std::vector<Cut> cuts{};
    bool by_models = false;  // false: no stretch was outlined, all were cut at pauses
};

// What the models make of one video: where its sections are, what each
// says, and what the whole says. What they could not do is noted in
// output(), as findings.
class Outliner {
public:
    Outliner(const StageContext& context, const Source& source, const Transcript& transcript,
             const reason::OutlineCompute& compute, agents::Army& army, Prompts prompts)
        : context_(&context)
        , transcript_(&transcript)
        , compute_(compute)
        , army_(&army)
        , prompts_(std::move(prompts))
        , shared_(video_shared(source.video)) {
        const agents::Engine* commander = engine_serving(army, "commander");
        if (commander != nullptr && commander->thinking.thinking_budget.has_value()) {
            closing_ = commander->thinking.thinking_budget->closing;
        }
        // Thinking takes half of what the commander holds at most: a budget
        // that left no room for the transcript would leave nothing to think about.
        thinking_budget_ = commander == nullptr
                               ? compute.thinking_budget
                               : std::min(compute.thinking_budget, commander->ctx_per_slot / 2);
        video_ = std::format("{}\n{}\n{}", shared_.at("title").get<std::string>(),
                             shared_.at("channel").get<std::string>(),
                             shared_.at("description").get<std::string>());
    }

    // The sections where the commander's outlines agree (the header
    // comment); a stretch none outlined is cut where the speaker paused.
    Result<Outlined> outlined() {
        RR_TRY_ASSIGN(WindowPlan plan, plan_windows(*transcript_, kWholeTranscript, *army_,
                                                    [this](const SentenceWindow& window, std::size_t number) {
                                                        return outline_mission(window, number);
                                                    }));
        progress(std::format("outline: the commander outlines {} {}, {} {}", plan.missions.size(),
                             plan.missions.size() == 1 ? "window" : "windows", compute_.samples,
                             compute_.samples == 1 ? "time" : "times each"));
        const std::vector<SentenceWindow> windows = plan.windows;
        RR_TRY_ASSIGN(const std::vector<agents::Outcome> outcomes,
                      army_->squad(std::move(plan.missions), context_->stop));
        // Every stretch in order: the windows read, and those set aside.
        std::vector<std::pair<SentenceWindow, const agents::Outcome*>> stretches;
        stretches.reserve(windows.size() + plan.too_long.size());
        for (std::size_t index = 0; index < windows.size(); ++index) {
            stretches.emplace_back(windows[index], &outcomes[index]);
        }
        for (const SentenceWindow& window : plan.too_long) {
            stretches.emplace_back(window, nullptr);
        }
        std::ranges::sort(stretches, {}, [](const auto& stretch) { return stretch.first.begin; });
        // Sentences set aside one after another are one stretch: an engine
        // that holds none of them cuts the video at pauses, not at every sentence.
        for (std::size_t index = 1; index < stretches.size();) {
            auto& before = stretches[index - 1];
            if (before.second == nullptr && stretches[index].second == nullptr &&
                before.first.end == stretches[index].first.begin) {
                before.first.end = stretches[index].first.end;
                stretches.erase(stretches.begin() + static_cast<std::ptrdiff_t>(index));
            } else {
                ++index;
            }
        }
        Outlined outlined;
        for (const auto& [window, outcome] : stretches) {
            std::vector<Cut> cuts = outcome == nullptr ? set_aside(window) : agreed(window, *outcome);
            outlined.by_models = outlined.by_models || (outcome != nullptr && !outcome->answers.empty());
            outlined.cuts.insert(outlined.cuts.end(), std::make_move_iterator(cuts.begin()),
                                 std::make_move_iterator(cuts.end()));
        }
        return outlined;
    }

    // Each section's description, in the order of `cuts`; none for a
    // section no answer described.
    Result<std::vector<std::optional<Description>>> described(std::span<const Cut> cuts) {
        std::vector<std::optional<Description>> descriptions(cuts.size());
        std::vector<std::size_t> asked;  // the sections asked about, in order
        std::vector<agents::Mission> missions;
        for (std::size_t index = 0; index < cuts.size(); ++index) {
            RR_TRY_ASSIGN(agents::Mission mission, describe_mission(cuts, index));
            if (!army_->holds(mission)) {
                note_ungisted(cuts, index, std::format("it is more than {} holds at once", model()));
                continue;
            }
            asked.push_back(index);
            missions.push_back(std::move(mission));
        }
        progress(std::format("outline: the commander describes {} {}", missions.size(),
                             missions.size() == 1 ? "section" : "sections"));
        RR_TRY_ASSIGN(const std::vector<agents::Outcome> outcomes,
                      army_->squad(std::move(missions), context_->stop));
        for (std::size_t at = 0; at < asked.size(); ++at) {
            const agents::Outcome& outcome = outcomes[at];
            note_ceiling(outcome);
            if (outcome.answers.empty()) {
                note_ungisted(cuts, asked[at], why_unanswered(outcome, *army_, "describe it"));
                continue;
            }
            descriptions[asked[at]] = description_of(outcome.answers.front().value);
        }
        return descriptions;
    }

    // The video's gist, from its sections' in order; none when no section
    // has one, or no answer wrote it.
    Result<std::optional<std::string>> video_gist(std::span<const Cut> cuts,
                                                  std::span<const std::optional<Description>> descriptions) {
        std::string lines;
        bool any = false;
        for (std::size_t index = 0; index < cuts.size(); ++index) {
            const std::optional<Description>& description = descriptions[index];
            any = any || description.has_value();
            lines += std::format("{}{}. [{}] {}{}", lines.empty() ? "" : "\n", index + 1,
                                 format_timestamp(transcript_->sentences[cuts[index].first].span.start()),
                                 cuts[index].title, description.has_value() ? ": " + description->gist : "");
        }
        if (!any) {
            output_.findings.emplace_back("the video has no gist: no section has one to write it from");
            return std::nullopt;
        }
        RR_TRY_ASSIGN(
            agents::Mission mission,
            mission_of("brief", prompts_.brief, json::Json{{"sections", lines}}, kBriefAnswerTokens));
        mission.check = [](const json::Json& answer) { return video_gist_problems_in(answer); };
        if (!army_->holds(mission)) {
            output_.findings.push_back(
                std::format("the video has no gist: its sections' are more than {} holds at once", model()));
            return std::nullopt;
        }
        RR_TRY_ASSIGN(const agents::Outcome outcome, army_->execute(std::move(mission), context_->stop));
        note_ceiling(outcome);
        if (outcome.answers.empty()) {
            output_.findings.push_back(
                std::format("the video has no gist: {}", why_unanswered(outcome, *army_, "write it")));
            return std::nullopt;
        }
        return std::optional<std::string>{outcome.answers.front().value.at("gist").get<std::string>()};
    }

    [[nodiscard]] StageOutput& output() { return output_; }

private:
    void progress(const std::string& line) const {
        if (context_->progress) {
            context_->progress(line);
        }
    }

    [[nodiscard]] std::string model() const {
        const agents::Engine* commander = engine_serving(*army_, "commander");
        return commander == nullptr ? std::string{"the commander"} : commander->model;
    }

    // A mission of `prompt` for the commander, without thinking, rendered
    // with `order`; its seed derived from its id.
    [[nodiscard]] Result<agents::Mission> mission_of(std::string id, const StagePrompt& prompt,
                                                     const json::Json& order, int answer_tokens) const {
        RR_TRY_ASSIGN(const prompt::Rendered rendered,
                      context_->prompts->render(prompt.info->name, shared_, order));
        const std::int64_t seed = llm::derived_seed(context_->job->salt, OutlineStage::kName, id, 0);
        return agents::Mission{.id = std::move(id),
                               .kind = prompt.info->name,
                               .prompt = prompt.info->label(),
                               .role = "commander",
                               .request = llm::ChatRequest{.messages = {llm::Message::system(rendered.system),
                                                                        llm::Message::user(rendered.user)},
                                                           .seed = seed,
                                                           .thinking = false},
                               .answer_tokens = answer_tokens,
                               .schema = prompt.schema};
    }

    [[nodiscard]] Result<agents::Mission> outline_mission(const SentenceWindow& window,
                                                          std::size_t number) const {
        const json::Json order{{"opening", window.begin == 0},
                               {"before", sentence_lines(*transcript_, window.context_begin, window.begin)},
                               {"part", part_of(*transcript_, window)},
                               {"sentences", sentence_lines(*transcript_, window.begin, window.end)}};
        RR_TRY_ASSIGN(agents::Mission mission, mission_of(std::format("outline-{}", number), prompts_.outline,
                                                          order, kOutlineAnswerTokens));
        mission.request.thinking = true;
        mission.request.thinking_budget =
            llm::ThinkingBudget{.tokens = thinking_budget_, .closing = closing_};
        mission.samples = compute_.samples;
        mission.check = [transcript = transcript_, window](const json::Json& answer) {
            return outline_problems_in(answer, *transcript, window);
        };
        return mission;
    }

    [[nodiscard]] Result<agents::Mission> describe_mission(std::span<const Cut> cuts,
                                                           std::size_t index) const {
        const std::size_t first = cuts[index].first;
        const std::size_t end =
            index + 1 < cuts.size() ? cuts[index + 1].first : transcript_->sentences.size();
        const SentenceWindow window = window_of(*transcript_, first, end);
        const json::Json order{{"before", sentence_lines(*transcript_, window.context_begin, first)},
                               {"section", cuts[index].title},
                               {"start", format_timestamp(transcript_->sentences[first].span.start())},
                               {"end", format_timestamp(transcript_->sentences[end - 1].span.end())},
                               {"sentences", sentence_lines(*transcript_, first, end)}};
        RR_TRY_ASSIGN(agents::Mission mission, mission_of(std::format("describe-{}", index + 1),
                                                          prompts_.describe, order, kDescribeAnswerTokens));
        mission.check = [said = said_by(*transcript_, first, end), video = video_](const json::Json& answer) {
            return description_problems_in(answer, said, video);
        };
        return mission;
    }

    // The cuts the samples of `window`'s outline agree on; when none was
    // accepted, `window` cut where the speaker paused, and a finding.
    std::vector<Cut> agreed(const SentenceWindow& window, const agents::Outcome& outcome) {
        note_ceiling(outcome);
        if (outcome.answers.empty()) {
            output_.findings.push_back(std::format("{} no outline: {}; cut where the speaker paused instead",
                                                   sentences_have(*transcript_, window.begin, window.end),
                                                   why_unanswered(outcome, *army_, "outline them")));
            return pause_cuts(*transcript_, window.begin, window.end);
        }
        std::vector<std::vector<Cut>> samples;
        samples.reserve(outcome.answers.size());
        for (const agents::Answer& answer : outcome.answers) {
            samples.push_back(cuts_of(answer.value));
        }
        return agreed_cuts(samples, compute_.boundary_tolerance, compute_.quorum);
    }

    // Sentences too long for the commander even alone: cut where the
    // speaker paused; one sentence starts a section, and the next window
    // says where the next one does.
    std::vector<Cut> set_aside(const SentenceWindow& stretch) {
        const bool one = stretch.end - stretch.begin == 1;
        output_.findings.push_back(
            std::format("{} no outline: even alone, {} more than {} holds at once; {}",
                        sentences_have(*transcript_, stretch.begin, stretch.end), one ? "it is" : "each is",
                        model(), one ? "a section starts there" : "cut where the speaker paused instead"));
        return pause_cuts(*transcript_, stretch.begin, stretch.end);
    }

    void note_ungisted(std::span<const Cut> cuts, std::size_t index, std::string_view why) {
        const std::size_t end =
            index + 1 < cuts.size() ? cuts[index + 1].first : transcript_->sentences.size();
        output_.findings.push_back(
            std::format("section {}, \"{}\" ({} to {}), has no gist: {}", index + 1, cuts[index].title,
                        format_timestamp(transcript_->sentences[cuts[index].first].span.start()),
                        format_timestamp(transcript_->sentences[end - 1].span.end()), why));
    }

    // After a ceiling, the result is this job's alone: a job allowed more
    // would have asked what this one could not.
    void note_ceiling(const agents::Outcome& outcome) {
        if (outcome.verdict == agents::Verdict::ceiling) {
            output_.reusable = false;
        }
    }

    const StageContext* context_;
    const Transcript* transcript_;
    reason::OutlineCompute compute_;
    agents::Army* army_;
    Prompts prompts_;
    json::Json shared_;
    std::string video_;        // the shared part's text, where a speaker may be named
    std::string closing_;      // the commander's words that end its thinking
    int thinking_budget_ = 0;  // an outline's (see the constructor)
    StageOutput output_;
};

StageOutput without_a_pod(const Source& source, const Transcript& transcript, bool chapters) {
    const std::vector<Cut> cuts = chapters
                                      ? cut_long(chapter_cuts(source.video.chapters, transcript), transcript)
                                      : pause_cuts(transcript, 0, transcript.sentences.size());
    const knowledge::Outline outline =
        outline_of(cuts, transcript, knowledge::Brief{},
                   chapters ? knowledge::SectionsFrom::chapters : knowledge::SectionsFrom::pauses);
    return StageOutput{.artifact = knowledge::to_json(outline),
                       .findings = {std::format(
                           kNoPod, chapters ? "the creator's chapters" : "cut where the speaker paused")}};
}

struct Read {
    Source source{};
    Transcript transcript{};
    reason::Tier tier{};
    bool chapters = false;  // the sections are the creator's chapters
};

Result<Read> read_input(const StageContext& context) {
    RR_ASSERT(context.job != nullptr, "a stage runs for a job");
    Read read;
    RR_TRY_ASSIGN(read.source, source_from_json(context.inputs.at(std::string{SourceStage::kName})));
    RR_TRY_ASSIGN(read.transcript,
                  knowledge::transcript_from_json(context.inputs.at(std::string{TranscriptStage::kName})));
    if (read.transcript.sentences.empty()) {
        return fail(Errc::invalid_argument, "the transcript has no sentences: there is nothing to outline");
    }
    RR_TRY_ASSIGN(read.tier, job_tier(context));
    read.chapters = read.tier.outline.chapters && read.source.video.chapters.size() >= kLeastChapters;
    return read;
}

}  // namespace

Result<json::Json> OutlineStage::fingerprint(const StageContext& context) const {
    RR_ASSERT(context.job != nullptr, "a stage runs for a job");
    RR_TRY_ASSIGN(const reason::Tier tier, job_tier(context));
    json::Json fingerprint{{"tier", tier.canonical(reason::Stage::outline)},
                           {"longest_ms", kLongestSection.count()},
                           {"models", nullptr}};
    if (!context.army) {
        return fingerprint;
    }
    RR_ASSERT(context.prompts != nullptr, "a stage that asks models is given the prompts");
    RR_TRY_ASSIGN(json::Json prompts, prompts_fingerprint(*context.prompts, {kOutline, kDescribe, kBrief}));
    RR_TRY_ASSIGN(agents::Army* const army, context.army());
    const agents::Engine* commander = engine_serving(*army, "commander");
    fingerprint["models"] =
        json::Json{{"prompts", std::move(prompts)},
                   {"army", army_fingerprint(*army)},
                   {"holds", commander == nullptr ? 0 : commander->ctx_per_slot},
                   {"context", json::Json{{"sentences", kContextSentences}, {"bytes", kContextBytes}}}};
    return fingerprint;
}

Result<StageOutput> OutlineStage::run(const StageContext& context) const {
    RR_TRY_ASSIGN(const Read read, read_input(context));
    if (!context.army) {
        return without_a_pod(read.source, read.transcript, read.chapters);
    }
    RR_TRY_ASSIGN(agents::Army* const army, context.army());
    RR_TRY_ASSIGN(Prompts prompts, prompts_of(*context.prompts));
    Outliner outliner{context, read.source, read.transcript, read.tier.outline, *army, std::move(prompts)};

    std::vector<Cut> cuts;
    knowledge::SectionsFrom from = knowledge::SectionsFrom::chapters;
    if (read.chapters) {
        cuts = chapter_cuts(read.source.video.chapters, read.transcript);
    } else {
        RR_TRY_ASSIGN(Outlined outlined, outliner.outlined());
        cuts = std::move(outlined.cuts);
        from = outlined.by_models ? knowledge::SectionsFrom::models : knowledge::SectionsFrom::pauses;
    }
    cuts = cut_long(std::move(cuts), read.transcript);

    RR_TRY_ASSIGN(const std::vector<std::optional<Description>> descriptions, outliner.described(cuts));
    std::vector<knowledge::Brief> briefs;
    briefs.reserve(descriptions.size());
    for (const std::optional<Description>& description : descriptions) {
        if (description.has_value()) {
            briefs.push_back(description->brief);
        }
    }
    knowledge::Brief brief = join_briefs(briefs);
    RR_TRY_ASSIGN(brief.gist, outliner.video_gist(cuts, descriptions));

    knowledge::Outline outline = outline_of(cuts, read.transcript, std::move(brief), from);
    for (std::size_t index = 0; index < descriptions.size(); ++index) {
        if (const std::optional<Description>& description = descriptions[index]; description.has_value()) {
            outline.sections[index].gist = description->gist;
        }
    }
    StageOutput output = std::move(outliner.output());
    output.artifact = knowledge::to_json(outline);
    return output;
}

}  // namespace rr::pipeline

// The stage that writes the article (rr/pipeline/write_stage.hpp).
#include <rr/pipeline/write_stage.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <rr/agents/mission.hpp>
#include <rr/core/assert.hpp>
#include <rr/core/time_span.hpp>
#include <rr/fidelity/checks.hpp>
#include <rr/knowledge/article.hpp>
#include <rr/knowledge/atoms.hpp>
#include <rr/knowledge/outline.hpp>
#include <rr/knowledge/plan.hpp>
#include <rr/knowledge/transcript.hpp>
#include <rr/llm/memo.hpp>
#include <rr/pipeline/asking.hpp>
#include <rr/pipeline/atomize_stage.hpp>
#include <rr/pipeline/outline_stage.hpp>
#include <rr/pipeline/plan_stage.hpp>
#include <rr/pipeline/source.hpp>
#include <rr/pipeline/transcript_stage.hpp>
#include <rr/reason/tier.hpp>
#include <rr/text/align.hpp>
#include <rr/text/span.hpp>
#include <rr/text/unicode.hpp>

namespace rr::pipeline {

namespace {

using knowledge::Atom;
using knowledge::AtomId;
using knowledge::Transcript;

constexpr std::string_view kPrompt = "write";
// An answer's size, in tokens: its atoms' words, told in at most twice as
// many, at the estimator's three bytes a token; each paragraph's markers and
// breaks; and room besides.
constexpr int kTokensPerParagraph = 64;
constexpr int kSpareTokens = 128;
constexpr int kLeastAnswer = 512;
constexpr int kMostAnswer = 8'192;
constexpr std::size_t kShownBytes = 120;  // of a sentence a message quotes

// A paragraph to write: its purpose, and its atoms in the order told.
struct ToWrite {
    std::optional<std::string> purpose{};
    std::vector<const Atom*> atoms{};
};

// A section of the plan to write, with the outline's for when it was said.
struct SectionToWrite {
    const knowledge::SectionPlan* plan = nullptr;
    const knowledge::Section* section = nullptr;
    std::vector<ToWrite> paragraphs{};
};

int answer_tokens(std::span<const ToWrite> paragraphs) {
    std::int64_t bytes = 0;
    for (const ToWrite& paragraph : paragraphs) {
        for (const Atom* const atom : paragraph.atoms) {
            bytes += static_cast<std::int64_t>(atom->text.size() + atom->quote.value_or("").size());
        }
    }
    const std::int64_t tokens = ((bytes * 2) / 3) +
                                (static_cast<std::int64_t>(paragraphs.size()) * kTokensPerParagraph) +
                                kSpareTokens;
    return static_cast<int>(std::clamp<std::int64_t>(tokens, kLeastAnswer, kMostAnswer));
}

// `text` cut to kShownBytes at a character's boundary, with "…" when cut.
std::string shown(std::string_view text) {
    if (text.size() <= kShownBytes) {
        return std::string{text};
    }
    std::size_t end = 0;
    for (const text::CodePoint& point : text::code_points(text)) {
        if (point.end > kShownBytes) {
            break;
        }
        end = point.end;
    }
    return std::string{text.substr(0, end)} + "\u2026";
}

// "A7 [1:02] opinion, aside, high, said by Ada: The speaker was surprised.
// To quote: "…". Names: …. Terms: ….": an atom as the prompt shows it, its
// labels before its words and what must reach the paragraph after them.
std::string atom_entry(const Atom& atom, const Transcript& transcript) {
    const knowledge::Sentence* said = transcript.find(atom.sentences.front());
    RR_ASSERT(said != nullptr, "an atom is said in its transcript (read_input())");
    const std::string_view kind = knowledge::to_string(atom.kind);
    std::string labels{kind};
    const auto label = [&](std::string_view more) {
        if (more != kind) {
            labels += std::format(", {}", more);
        }
    };
    if (atom.modality != knowledge::Modality::asserted) {
        label(knowledge::to_string(atom.modality));
    }
    if (atom.register_kind != knowledge::Register::main) {
        label(knowledge::to_string(atom.register_kind));
    }
    if (atom.intensity != knowledge::Intensity::medium) {
        label(knowledge::to_string(atom.intensity));
    }
    if (atom.speaker.has_value()) {
        labels += ", said by " + *atom.speaker;
    }
    if (atom.attributed_to.has_value()) {
        labels += ", reporting " + *atom.attributed_to;
    }
    std::string entry =
        std::format("{} [{}] {}: {}", atom.id, format_timestamp(said->span.start()), labels, atom.text);
    if (atom.quote.has_value()) {
        entry += std::format(" To quote: \"{}\".", *atom.quote);
    }
    const auto listed = [&](std::string_view what, const std::vector<std::string>& words) {
        if (!words.empty()) {
            std::string list;
            for (const std::string& word : words) {
                list += (list.empty() ? "" : ", ") + word;
            }
            entry += std::format(" {}: {}.", what, list);
        }
    };
    listed("Names", atom.names);
    listed("Terms", atom.terms);
    return entry;
}

// --- Checks -------------------------------------------------------------------------

// What a mission was asked to write: each paragraph's atoms, in order.
struct Asked {
    std::vector<std::vector<AtomId>> paragraphs{};
    std::vector<AtomId> atoms{};  // all of them, in order: what Ledger B holds the draft to
    std::size_t first = 1;        // the number of the first paragraph asked, in its section
};

// The draft's blocks, read: its paragraphs, and whether it wrote anything else.
struct Read {
    knowledge::ParsedArticle parsed{};
    std::vector<const knowledge::Block*> paragraphs{};
    bool headed = false;
    bool other = false;  // a list, a quote set apart, code
};

Read read_draft(std::string_view draft) {
    Read read{.parsed = knowledge::parse_article(draft)};
    for (const knowledge::Block& block : read.parsed.blocks) {
        switch (block.kind) {
            case knowledge::BlockKind::paragraph:
                read.paragraphs.push_back(&block);
                break;
            case knowledge::BlockKind::heading:
                read.headed = true;
                break;
            case knowledge::BlockKind::list_item:
            case knowledge::BlockKind::quote:
            case knowledge::BlockKind::code:
                read.other = true;
                break;
        }
    }
    return read;
}

// The paragraph, of `read`'s, that first cites `atom`; none when none does.
std::optional<std::size_t> paragraph_citing(const Read& read, AtomId atom) {
    for (std::size_t index = 0; index < read.paragraphs.size(); ++index) {
        for (const knowledge::CitingSentence& sentence : read.paragraphs[index]->sentences) {
            if (std::ranges::any_of(sentence.markers,
                                    [&](const knowledge::Marker& marker) { return marker.atom == atom; })) {
                return index;
            }
        }
    }
    return std::nullopt;
}

void check_shape(const Read& read, const Asked& asked, Problems& problems) {
    if (read.headed) {
        problems.note("write no heading: the article has its own");
    }
    if (read.other) {
        problems.note("write paragraphs of prose only: no lists, no quotes set apart, no code");
    }
    if (read.paragraphs.size() != asked.paragraphs.size()) {
        problems.note(std::format("write {} paragraphs, one for each given, not {}", asked.paragraphs.size(),
                                  read.paragraphs.size()));
        return;  // which paragraph is which cannot be told
    }
    for (std::size_t index = 0; index < asked.paragraphs.size(); ++index) {
        for (const AtomId atom : asked.paragraphs[index]) {
            const std::optional<std::size_t> found = paragraph_citing(read, atom);
            if (found.has_value() && *found != index) {
                problems.note(std::format("{} is told in paragraph {}: tell it in paragraph {}, as given",
                                          atom, asked.first + *found, asked.first + index));
            }
        }
    }
}

// What is wrong with `draft` as the paragraphs `asked` (the header comment's
// step 2).
std::vector<std::string> draft_problems(std::string_view draft, const Transcript& transcript,
                                        const knowledge::AtomLedger& ledger, const Asked& asked) {
    Problems problems;
    const Read read = read_draft(draft);
    check_shape(read, asked, problems);
    for (const knowledge::Block* const paragraph : read.paragraphs) {
        for (const knowledge::CitingSentence& sentence : paragraph->sentences) {
            if (sentence.markers.empty()) {
                problems.note(
                    std::format("the sentence \"{}\" tells no atom: cite the atoms it tells, or leave it out",
                                shown(knowledge::without_markers(draft, read.parsed, sentence.span))));
            }
        }
    }
    for (const fidelity::Finding& finding :
         fidelity::check_article(transcript, ledger, draft, {.told = asked.atoms}).findings) {
        if (finding.severity == fidelity::Severity::error) {
            problems.note(finding.message);
        }
    }
    return std::move(problems).list();
}

// The warnings of a draft that passed: what decides between drafts.
std::size_t warnings_of(std::string_view draft, const Transcript& transcript,
                        const knowledge::AtomLedger& ledger, const Asked& asked) {
    return fidelity::check_article(transcript, ledger, draft, {.told = asked.atoms}).warnings();
}

// The paragraphs of an accepted draft, each as written, without the space
// around them.
std::vector<std::string> paragraphs_of(std::string_view draft) {
    const Read read = read_draft(draft);
    std::vector<std::string> paragraphs;
    paragraphs.reserve(read.paragraphs.size());
    for (const knowledge::Block* const paragraph : read.paragraphs) {
        paragraphs.emplace_back(text::text_of(draft, paragraph->span));
    }
    return paragraphs;
}

// `paragraph` told as its atoms say it: each atom's words, its words to
// quote where they are not among them, and its marker.
std::string told_as_said(const ToWrite& paragraph) {
    std::string told;
    for (const Atom* const atom : paragraph.atoms) {
        std::string sentence = atom->text;
        if (atom->quote.has_value() && !text::align(*atom->quote, atom->text).verbatim()) {
            sentence += std::format(" (\"{}\")", *atom->quote);
        }
        told += std::format("{}{} [{}]", told.empty() ? "" : " ", sentence, atom->id);
    }
    return told;
}

// --- The models' part ----------------------------------------------------------------

// Paragraphs [begin, end) of a section, written in one mission, or set
// aside: more than the commander holds even one at a time.
struct Stretch {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::optional<std::size_t> mission{};  // its index in the squad; none: set aside
};

// What of a section of `count` paragraphs is told as its atoms say it, when
// `stretch` is: the section, a paragraph of it, or a few.
std::string told_plainly(const Stretch& stretch, std::size_t count) {
    if (stretch.end - stretch.begin == count) {
        return "it is told as its atoms say it";
    }
    if (stretch.end - stretch.begin == 1) {
        return std::format("paragraph {} is told as its atoms say it", stretch.begin + 1);
    }
    return std::format("paragraphs {} to {} are told as their atoms say it", stretch.begin + 1, stretch.end);
}

// What the commander makes of the sections: each written, in parts where it
// does not hold one whole, or told as its atoms say it where no draft
// passed. What it could not do is noted in output(), as findings.
class Writer {
public:
    Writer(const StageContext& context, const Transcript& transcript, const knowledge::AtomLedger& ledger,
           const reason::WriteCompute& compute, agents::Army& army, StagePrompt prompt, json::Json shared)
        : context_(&context)
        , transcript_(&transcript)
        , ledger_(&ledger)
        , compute_(compute)
        , army_(&army)
        , prompt_(std::move(prompt))
        , shared_(std::move(shared)) {
        // A tier that writes thinking thinks with the commander's own budget,
        // within half of what it holds (the outline's rule).
        const agents::Engine* commander = engine_serving(army, "commander");
        if (commander != nullptr && commander->thinking.thinking_budget.has_value()) {
            budget_ = commander->thinking.thinking_budget;
            budget_->tokens = std::min(budget_->tokens, commander->ctx_per_slot / 2);
        }
    }

    // Each section's paragraphs, in order: none for a section with none.
    Result<std::vector<std::vector<std::string>>> write(std::span<const SectionToWrite> sections) {
        std::vector<std::vector<Stretch>> stretches(sections.size());
        std::vector<agents::Mission> missions;
        std::size_t written = 0;  // sections the commander is asked for, whole or in part
        for (std::size_t index = 0; index < sections.size(); ++index) {
            const std::size_t before = missions.size();
            RR_TRY_ASSIGN(stretches[index], stretches_of(sections[index], index + 1, missions));
            if (missions.size() > before) {
                ++written;
            }
        }
        if (!missions.empty()) {
            progress(std::format(
                "write: the commander writes {} {}, {} {}{}", written, written == 1 ? "section" : "sections",
                compute_.drafts, compute_.drafts == 1 ? "draft" : "drafts each",
                missions.size() > written ? std::format(", in {} parts", missions.size()) : ""));
        }
        RR_TRY_ASSIGN(const std::vector<agents::Outcome> outcomes,
                      army_->squad(std::move(missions), context_->stop));
        std::vector<std::vector<std::string>> paragraphs(sections.size());
        for (std::size_t index = 0; index < sections.size(); ++index) {
            for (const Stretch& stretch : stretches[index]) {
                std::ranges::move(assembled(sections[index], index + 1, stretch, outcomes),
                                  std::back_inserter(paragraphs[index]));
            }
        }
        return paragraphs;
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

    // The stretches of `section`'s paragraphs (see Stretch), each the most
    // the commander holds from where the last one ended; their missions are
    // added to `missions`.
    [[nodiscard]] Result<std::vector<Stretch>> stretches_of(const SectionToWrite& section, std::size_t number,
                                                            std::vector<agents::Mission>& missions) const {
        std::vector<Stretch> stretches;
        std::size_t parts = 0;  // missions of the section so far
        for (std::size_t begin = 0; begin < section.paragraphs.size();) {
            RR_TRY_ASSIGN(Held held, most_held(section, number, parts + 1, begin));
            if (!held.mission.has_value()) {
                // Set aside, with any set aside just before: told as said.
                if (!stretches.empty() && !stretches.back().mission.has_value()) {
                    stretches.back().end = begin + 1;
                } else {
                    stretches.push_back(Stretch{.begin = begin, .end = begin + 1});
                }
                ++begin;
                continue;
            }
            stretches.push_back(Stretch{.begin = begin, .end = held.end, .mission = missions.size()});
            missions.push_back(std::move(*held.mission));
            ++parts;
            begin = held.end;
        }
        return stretches;
    }

    // The mission writing the most paragraphs of `section` from `begin` that
    // the commander holds, its `part`th, and where its paragraphs end; no
    // mission when it holds not even the paragraph at `begin`.
    struct Held {
        std::size_t end = 0;
        std::optional<agents::Mission> mission{};
    };

    [[nodiscard]] Result<Held> most_held(const SectionToWrite& section, std::size_t number, std::size_t part,
                                         std::size_t begin) const {
        const std::size_t count = section.paragraphs.size();
        const auto id = [&](std::size_t end) {
            return begin == 0 && end == count ? std::format("write-{}", number)
                                              : std::format("write-{}-{}", number, part);
        };
        RR_TRY_ASSIGN(agents::Mission whole, mission_of(id(count), section, number, begin, count));
        if (army_->holds(whole)) {
            return Held{.end = count, .mission = std::move(whole)};
        }
        // Bisect for the most paragraphs the commander holds: `held` it holds
        // (or is `begin`, none), `refused` it does not.
        std::size_t held = begin;
        std::size_t refused = count;
        while (refused - held > 1) {
            const std::size_t middle = held + ((refused - held) / 2);
            RR_TRY_ASSIGN(const agents::Mission trial,
                          mission_of(id(middle), section, number, begin, middle));
            (army_->holds(trial) ? held : refused) = middle;
        }
        if (held == begin) {
            return Held{.end = begin};
        }
        RR_TRY_ASSIGN(agents::Mission mission, mission_of(id(held), section, number, begin, held));
        return Held{.end = held, .mission = std::move(mission)};
    }

    // A mission to write paragraphs [begin, end) of `section`, the `number`th.
    [[nodiscard]] Result<agents::Mission> mission_of(std::string id, const SectionToWrite& section,
                                                     std::size_t number, std::size_t begin,
                                                     std::size_t end) const {
        const std::span<const ToWrite> paragraphs = std::span{section.paragraphs}.subspan(begin, end - begin);
        Asked asked{.first = begin + 1};
        std::string lines;
        for (std::size_t index = 0; index < paragraphs.size(); ++index) {
            const ToWrite& paragraph = paragraphs[index];
            lines += std::format("{}Paragraph {}: {}", lines.empty() ? "" : "\n", begin + index + 1,
                                 paragraph.purpose.value_or("Tell its atoms in the order given."));
            asked.paragraphs.emplace_back();
            for (const Atom* const atom : paragraph.atoms) {
                lines += "\n" + atom_entry(*atom, *transcript_);
                asked.paragraphs.back().push_back(atom->id);
                asked.atoms.push_back(atom->id);
            }
        }
        const bool whole = begin == 0 && end == section.paragraphs.size();
        const json::Json order{
            {"number", number},
            {"heading", section.plan->heading},
            {"start", format_timestamp(sentence(section.section->first).span.start())},
            {"end", format_timestamp(sentence(section.section->last).span.end())},
            {"part", whole
                         ? std::string{}
                         : std::format("Its paragraphs are too many to write at once: write paragraphs {} to "
                                       "{} here; the others are written apart.",
                                       begin + 1, end)},
            {"paragraphs", std::move(lines)}};
        RR_TRY_ASSIGN(const prompt::Rendered rendered, context_->prompts->render(kPrompt, shared_, order));
        const std::int64_t seed = llm::derived_seed(context_->job->salt, WriteStage::kName, id, 0);
        agents::Mission mission{
            .id = std::move(id),
            .kind = std::string{kPrompt},
            .prompt = prompt_.info->label(),
            .role = "commander",
            .request = llm::ChatRequest{.messages = {llm::Message::system(rendered.system),
                                                     llm::Message::user(rendered.user)},
                                        .seed = seed,
                                        .thinking = compute_.thinking},
            .answer_tokens = answer_tokens(paragraphs),
            .check =
                [transcript = transcript_, ledger = ledger_,
                 asked = std::move(asked)](const json::Json& answer) {
                    return draft_problems(answer.get<std::string>(), *transcript, *ledger, asked);
                },
            .repairs = compute_.repair_rounds,
            .samples = compute_.drafts};
        if (compute_.thinking) {
            mission.request.thinking_budget = budget_;
        }
        return mission;
    }

    [[nodiscard]] const knowledge::Sentence& sentence(knowledge::SentenceId id) const {
        const knowledge::Sentence* found = transcript_->find(id);
        RR_ASSERT(found != nullptr, "the outline covers its transcript (read_input())");
        return *found;
    }

    // The paragraphs of `stretch`: the draft kept, or, where no draft passed,
    // told as said, and said.
    std::vector<std::string> assembled(const SectionToWrite& section, std::size_t number,
                                       const Stretch& stretch, std::span<const agents::Outcome> outcomes) {
        const std::span<const ToWrite> paragraphs =
            std::span{section.paragraphs}.subspan(stretch.begin, stretch.end - stretch.begin);
        const agents::Outcome* const outcome =
            stretch.mission.has_value() ? &outcomes[*stretch.mission] : nullptr;
        if (outcome != nullptr && outcome->verdict == agents::Verdict::ceiling) {
            output_.reusable = false;  // a job allowed more would have written it
        }
        if (outcome == nullptr || outcome->answers.empty()) {
            const std::string why =
                outcome == nullptr
                    ? std::format("even alone, {} more than {} holds at once",
                                  paragraphs.size() == 1 ? "it is" : "each paragraph is", model())
                    : why_unanswered(*outcome, *army_, "write it");
            output_.findings.push_back(std::format("section {}, \"{}\": {}: {}", number,
                                                   section.plan->heading,
                                                   told_plainly(stretch, section.paragraphs.size()), why));
            std::vector<std::string> told;
            told.reserve(paragraphs.size());
            for (const ToWrite& paragraph : paragraphs) {
                told.push_back(told_as_said(paragraph));
            }
            return told;
        }
        // The draft with fewest warnings; the first, on a tie.
        Asked asked;
        for (const ToWrite& paragraph : paragraphs) {
            for (const Atom* const atom : paragraph.atoms) {
                asked.atoms.push_back(atom->id);
            }
        }
        const agents::Answer* kept = nullptr;
        std::size_t fewest = 0;
        for (const agents::Answer& answer : outcome->answers) {
            const std::size_t warnings =
                warnings_of(answer.value.get<std::string>(), *transcript_, *ledger_, asked);
            if (kept == nullptr || warnings < fewest) {
                kept = &answer;
                fewest = warnings;
            }
        }
        return paragraphs_of(kept->value.get<std::string>());
    }

    const StageContext* context_;
    const Transcript* transcript_;
    const knowledge::AtomLedger* ledger_;
    reason::WriteCompute compute_;
    agents::Army* army_;
    StagePrompt prompt_;
    json::Json shared_;
    std::optional<llm::ThinkingBudget> budget_;  // a tier that writes thinking: the commander's
    StageOutput output_;
};

// What the prompt's system part shows: the video, its gist, speakers and
// words, and the article's sections, the same for every section written.
json::Json shared_of(const Source& source, const Transcript& transcript, const knowledge::Outline& outline,
                     const knowledge::Plan& plan) {
    std::string speakers;
    for (const knowledge::Speaker& speaker : outline.brief.speakers) {
        speakers += std::format("{}- {}{}", speakers.empty() ? "" : "\n", speaker.name,
                                speaker.role.has_value() ? ", " + *speaker.role : "");
    }
    std::string lexicon;
    for (const knowledge::LexiconTerm& term : outline.brief.lexicon) {
        lexicon += std::format("{}- {}{}", lexicon.empty() ? "" : "\n", term.term,
                               term.meaning.has_value() ? ": " + *term.meaning : "");
    }
    std::string sections;
    std::size_t number = 0;
    for (const knowledge::SectionPlan& section : plan.sections) {
        const knowledge::Sentence* first = transcript.find(section.first);
        RR_ASSERT(first != nullptr, "the plan's sections are the outline's (read_input())");
        sections += std::format("{}{}. [{}] {}", sections.empty() ? "" : "\n", ++number,
                                format_timestamp(first->span.start()), section.heading);
    }
    const json::Json video = video_shared(source.video);
    return json::Json{{"title", video.at("title")},
                      {"channel", video.at("channel")},
                      {"gist", outline.brief.gist.value_or("")},
                      {"speakers", std::move(speakers)},
                      {"lexicon", std::move(lexicon)},
                      {"sections", std::move(sections)}};
}

struct Input {
    Source source{};
    Transcript transcript{};
    knowledge::Outline outline{};
    knowledge::AtomLedger ledger{};
    knowledge::Plan plan{};
    reason::Tier tier{};
};

Result<Input> read_input(const StageContext& context) {
    RR_ASSERT(context.job != nullptr, "a stage runs for a job");
    Input read;
    RR_TRY_ASSIGN(read.source, source_from_json(context.inputs.at(std::string{SourceStage::kName})));
    RR_TRY_ASSIGN(read.transcript,
                  knowledge::transcript_from_json(context.inputs.at(std::string{TranscriptStage::kName})));
    RR_TRY_ASSIGN(read.outline,
                  knowledge::outline_from_json(context.inputs.at(std::string{OutlineStage::kName})));
    if (const std::vector<std::string> problems = knowledge::outline_problems(read.outline, read.transcript);
        !problems.empty()) {
        return fail(Errc::internal,
                    std::format("the outline does not cover its transcript: {}", problems.front()));
    }
    RR_TRY_ASSIGN(read.ledger,
                  knowledge::atom_ledger_from_json(context.inputs.at(std::string{AtomizeStage::kName})));
    RR_TRY_ASSIGN(read.plan, knowledge::plan_from_json(context.inputs.at(std::string{PlanStage::kName})));
    if (const std::vector<std::string> problems =
            knowledge::plan_problems(read.plan, read.ledger, read.outline);
        !problems.empty()) {
        return fail(Errc::internal, std::format("the plan does not hold its ledger: {}", problems.front()));
    }
    RR_TRY_ASSIGN(read.tier, job_tier(context));
    return read;
}

// The plan's sections, each with the outline's and its atoms in full.
std::vector<SectionToWrite> sections_of(const Input& read) {
    std::vector<SectionToWrite> sections;
    sections.reserve(read.plan.sections.size());
    for (const knowledge::SectionPlan& plan : read.plan.sections) {
        const auto outlined =
            std::ranges::find(read.outline.sections, plan.first, &knowledge::Section::first);
        RR_ASSERT(outlined != read.outline.sections.end(),
                  "the plan's sections are the outline's (read_input())");
        SectionToWrite section{.plan = &plan, .section = &*outlined};
        section.paragraphs.reserve(plan.paragraphs.size());
        for (const knowledge::Paragraph& paragraph : plan.paragraphs) {
            ToWrite to_write{.purpose = paragraph.purpose};
            to_write.atoms.reserve(paragraph.atoms.size());
            for (const AtomId id : paragraph.atoms) {
                const Atom* const atom = read.ledger.find(id);
                RR_ASSERT(atom != nullptr, "the plan's atoms are the ledger's (read_input())");
                to_write.atoms.push_back(atom);
            }
            section.paragraphs.push_back(std::move(to_write));
        }
        sections.push_back(std::move(section));
    }
    return sections;
}

// The article: each section with paragraphs under its heading, in order.
std::string article_of(std::span<const SectionToWrite> sections,
                       std::span<const std::vector<std::string>> paragraphs) {
    std::string markdown;
    for (std::size_t index = 0; index < sections.size(); ++index) {
        if (paragraphs[index].empty()) {
            continue;
        }
        markdown += std::format("{}## {}\n", markdown.empty() ? "" : "\n", sections[index].plan->heading);
        for (const std::string& paragraph : paragraphs[index]) {
            markdown += "\n" + paragraph + "\n";
        }
    }
    return markdown;
}

}  // namespace

Result<json::Json> WriteStage::fingerprint(const StageContext& context) const {
    RR_ASSERT(context.job != nullptr, "a stage runs for a job");
    RR_TRY_ASSIGN(const reason::Tier tier, job_tier(context));
    if (!context.army) {
        return fail(Errc::needs_user,
                    "writing the article needs models, and no pod is configured: rreport doctor says how to "
                    "set one up");
    }
    RR_ASSERT(context.prompts != nullptr, "a stage that asks models is given the prompts");
    RR_TRY_ASSIGN(json::Json prompts, prompts_fingerprint(*context.prompts, {kPrompt}));
    RR_TRY_ASSIGN(agents::Army* const army, context.army());
    const agents::Engine* commander = engine_serving(*army, "commander");
    return json::Json{{"tier", tier.canonical(reason::Stage::write)},
                      {"prompts", std::move(prompts)},
                      {"army", army_fingerprint(*army)},
                      {"holds", commander == nullptr ? 0 : commander->ctx_per_slot}};
}

Result<StageOutput> WriteStage::run(const StageContext& context) const {
    RR_TRY_ASSIGN(const Input read, read_input(context));
    RR_TRY_ASSIGN(agents::Army* const army, context.army());
    RR_TRY_ASSIGN(StagePrompt prompt, stage_prompt(*context.prompts, kPrompt));
    const std::vector<SectionToWrite> sections = sections_of(read);
    Writer writer{context,
                  read.transcript,
                  read.ledger,
                  read.tier.write,
                  *army,
                  std::move(prompt),
                  shared_of(read.source, read.transcript, read.outline, read.plan)};
    RR_TRY_ASSIGN(const std::vector<std::vector<std::string>> paragraphs, writer.write(sections));
    const std::string markdown = article_of(sections, paragraphs);
    StageOutput output = std::move(writer.output());
    if (markdown.empty()) {
        output.findings.emplace_back(
            "the article is empty: the video says nothing to retell, every sentence "
            "is filler");
    }
    for (const fidelity::Finding& finding :
         fidelity::check_article(read.transcript, read.ledger, markdown, {.duplicates = read.plan.duplicates})
             .findings) {
        output.findings.push_back(finding.message);
    }
    output.artifact = knowledge::to_json(knowledge::Article{.markdown = markdown});
    return output;
}

}  // namespace rr::pipeline

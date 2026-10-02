// The stage that plans how the article retells the atoms
// (rr/pipeline/plan_stage.hpp).
#include <rr/pipeline/plan_stage.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <rr/agents/mission.hpp>
#include <rr/core/assert.hpp>
#include <rr/core/time_span.hpp>
#include <rr/fidelity/checks.hpp>
#include <rr/knowledge/atoms.hpp>
#include <rr/knowledge/outline.hpp>
#include <rr/knowledge/plan.hpp>
#include <rr/knowledge/transcript.hpp>
#include <rr/llm/memo.hpp>
#include <rr/pipeline/asking.hpp>
#include <rr/pipeline/atomize_stage.hpp>
#include <rr/pipeline/grouping.hpp>
#include <rr/pipeline/outline_stage.hpp>
#include <rr/pipeline/source.hpp>
#include <rr/pipeline/transcript_stage.hpp>
#include <rr/reason/tier.hpp>
#include <rr/text/tokens.hpp>

namespace rr::pipeline {

namespace {

using knowledge::Atom;
using knowledge::AtomId;
using knowledge::Transcript;

constexpr std::string_view kPrompt = "plan";
constexpr std::size_t kHeadingWords = 12;
constexpr std::size_t kPurposeWords = 30;  // one sentence
// An answer's size, in tokens: each atom's ID and, at worst, a paragraph's
// purpose of its own; and room besides.
constexpr int kTokensPerAtom = 48;
constexpr int kSpareTokens = 128;
constexpr int kLeastAnswer = 512;
constexpr int kMostAnswer = 8'192;

std::size_t words_in(const json::Json& text) {
    return text::terms(text.get<std::string>()).size();
}

// The schema allows A1, A2, … only.
AtomId atom_of(const json::Json& written) {
    const std::optional<AtomId> id = AtomId::parse(written.get<std::string>());
    RR_ASSERT(id.has_value(), "the answer's schema allows A1, A2, \u2026 only");
    return *id;
}

int answer_tokens(std::size_t atoms) {
    const std::int64_t tokens = (static_cast<std::int64_t>(atoms) * kTokensPerAtom) + kSpareTokens;
    return static_cast<int>(std::clamp<std::int64_t>(tokens, kLeastAnswer, kMostAnswer));
}

// "A12" or "A12–A40": atoms as a message names them.
std::string named(std::span<const Atom* const> atoms) {
    RR_ASSERT(!atoms.empty(), "a stretch holds an atom at least");
    return atoms.size() == 1 ? atoms.front()->id.str()
                             : std::format("{}\u2013{}", atoms.front()->id, atoms.back()->id);
}

// "A12 [4:12] claim, hedged, said by Ada: Maybe the scales are older than
// the lab.": an atom as the prompt shows it, its labels before its words.
std::string atom_line(const Atom& atom, const Transcript& transcript) {
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
    if (atom.speaker.has_value()) {
        labels += ", said by " + *atom.speaker;
    }
    if (atom.attributed_to.has_value()) {
        labels += ", reporting " + *atom.attributed_to;
    }
    std::string line =
        std::format("{} [{}] {}: {}", atom.id, format_timestamp(said->span.start()), labels, atom.text);
    if (atom.quote.has_value()) {
        line += std::format(" (to quote: \"{}\")", *atom.quote);
    }
    return line;
}

// --- Checks -------------------------------------------------------------------------

// What a mission was asked to plan: its atoms, in the order said.
struct Asked {
    std::vector<const Atom*> atoms{};
    std::string range{};  // as messages name them: "A12–A40"
};

void check_paragraphs(const json::Json& answer, const std::map<AtomId, const Atom*>& own, const Asked& asked,
                      std::set<AtomId>& told, Problems& problems) {
    std::size_t number = 0;
    for (const json::Json& paragraph : answer.at("paragraphs")) {
        ++number;
        const json::Json& atoms = paragraph.at("atoms");
        if (atoms.size() > kParagraphAtoms) {
            problems.note(std::format("paragraph {} tells {} atoms: split it, {} at most", number,
                                      atoms.size(), kParagraphAtoms));
        }
        if (const std::size_t words = words_in(paragraph.at("purpose")); words > kPurposeWords) {
            problems.note(
                std::format("paragraph {}'s purpose has {} words: say what a reader learns in one "
                            "sentence, {} words at most",
                            number, words, kPurposeWords));
        }
        for (const json::Json& written : atoms) {
            const AtomId id = atom_of(written);
            if (!own.contains(id)) {
                problems.note(
                    std::format("{} is not among the atoms to plan ({}): plan those only", id, asked.range));
            } else if (!told.insert(id).second) {
                problems.note(std::format("{} is told twice: tell each atom once, in one paragraph", id));
            }
        }
    }
}

void check_duplicates(const json::Json& answer, const std::map<AtomId, const Atom*>& own, const Asked& asked,
                      const std::set<AtomId>& told, std::set<AtomId>& declared, Problems& problems) {
    for (const json::Json& duplicate : answer.at("duplicates")) {
        const AtomId id = atom_of(duplicate.at("atom"));
        const AtomId of = atom_of(duplicate.at("of"));
        if (!own.contains(id)) {
            problems.note(
                std::format("{} is not among the atoms to plan ({}): declare those only", id, asked.range));
        } else if (id == of) {
            problems.note(std::format(
                "{} is declared a duplicate of itself: name the atom that tells what it says, or tell it",
                id));
        } else if (told.contains(id)) {
            problems.note(std::format("{} is told and declared a duplicate: it is one or the other", id));
        } else if (!declared.insert(id).second) {
            problems.note(std::format("{} is declared a duplicate twice: once is enough", id));
        } else if (!told.contains(of)) {
            problems.note(
                std::format("{} is declared a duplicate of {}, which no paragraph tells: name an atom "
                            "a paragraph tells, or tell it",
                            id, of));
        } else {
            for (const std::string& lost : fidelity::duplicate_problems(*own.at(id), *own.at(of))) {
                problems.note(std::format("{} is declared a duplicate of {}, yet {}: tell it", id, of, lost));
            }
        }
    }
}

// What is wrong with `answer` as a plan of the atoms `asked` (the header
// comment's step 2).
std::vector<std::string> answer_problems(const json::Json& answer, const Asked& asked) {
    Problems problems;
    std::map<AtomId, const Atom*> own;
    for (const Atom* const atom : asked.atoms) {
        own.emplace(atom->id, atom);
    }
    std::set<AtomId> told;
    check_paragraphs(answer, own, asked, told, problems);
    std::set<AtomId> declared;
    check_duplicates(answer, own, asked, told, declared, problems);
    for (const Atom* const atom : asked.atoms) {
        if (!told.contains(atom->id) && !declared.contains(atom->id)) {
            problems.note(
                std::format("{} is in no paragraph: tell it, or declare it a duplicate of the atom "
                            "that says all it says",
                            atom->id));
        }
    }
    if (const std::size_t words = words_in(answer.at("heading")); words > kHeadingWords) {
        problems.note(std::format("the heading has {} words: {} at most", words, kHeadingWords));
    }
    return std::move(problems).list();
}

// --- Answers, read ----------------------------------------------------------------

// An accepted answer, read: what one mission planned.
struct Planned {
    std::vector<knowledge::Paragraph> paragraphs{};
    std::vector<knowledge::Duplicate> duplicates{};
    std::string heading{};
};

Planned planned_of(const json::Json& answer) {
    Planned planned{.heading = answer.at("heading").get<std::string>()};
    for (const json::Json& written : answer.at("paragraphs")) {
        knowledge::Paragraph paragraph{.purpose = written.at("purpose").get<std::string>()};
        for (const json::Json& id : written.at("atoms")) {
            paragraph.atoms.push_back(atom_of(id));
        }
        planned.paragraphs.push_back(std::move(paragraph));
    }
    for (const json::Json& written : answer.at("duplicates")) {
        planned.duplicates.push_back(
            knowledge::Duplicate{.atom = atom_of(written.at("atom")),
                                 .of = atom_of(written.at("of")),
                                 .because = written.at("because").get<std::string>()});
    }
    return planned;
}

// --- The models' part ----------------------------------------------------------------

// A section of the outline, and the atoms whose first sentence it holds,
// in the order said.
struct SectionAtoms {
    const knowledge::Section* section = nullptr;
    std::vector<const Atom*> atoms{};
};

std::vector<SectionAtoms> sections_of(const knowledge::Outline& outline,
                                      const knowledge::AtomLedger& ledger) {
    std::vector<SectionAtoms> sections;
    sections.reserve(outline.sections.size());
    for (const knowledge::Section& section : outline.sections) {
        sections.push_back(SectionAtoms{.section = &section});
    }
    for (const Atom& atom : ledger.atoms) {
        const knowledge::SentenceId said = atom.sentences.front();
        const auto holding = std::ranges::find_if(
            sections, [&](const SectionAtoms& section) { return said <= section.section->last; });
        RR_ASSERT(holding != sections.end(), "every atom is said in a section (read_input())");
        holding->atoms.push_back(&atom);
    }
    return sections;
}

knowledge::SectionPlan grouped_section(const SectionAtoms& section) {
    return knowledge::SectionPlan{.first = section.section->first,
                                  .heading = section.section->title,
                                  .paragraphs = grouped(section.atoms),
                                  .planned_by = knowledge::PlannedBy::grouping};
}

// Atoms [begin, end) of a section, asked about in one mission, or set aside:
// more than the commander holds even one at a time.
struct Stretch {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::optional<std::size_t> mission{};  // its index in the squad; none: set aside
};

// What the commander makes of the sections: each planned, in parts where
// it does not hold one whole, or grouped where no answer planned it. What
// it could not do is noted in output(), as findings.
class Planner {
public:
    Planner(const StageContext& context, const Transcript& transcript, const reason::PlanCompute& compute,
            agents::Army& army, StagePrompt prompt, json::Json shared)
        : context_(&context)
        , transcript_(&transcript)
        , army_(&army)
        , prompt_(std::move(prompt))
        , shared_(std::move(shared)) {
        const agents::Engine* commander = engine_serving(army, "commander");
        if (commander != nullptr && commander->thinking.thinking_budget.has_value()) {
            closing_ = commander->thinking.thinking_budget->closing;
        }
        // Thinking takes half of what the commander holds at most: a budget
        // that left no room for the atoms would leave nothing to think about.
        thinking_budget_ = commander == nullptr
                               ? compute.thinking_budget
                               : std::min(compute.thinking_budget, commander->ctx_per_slot / 2);
    }

    Result<knowledge::Plan> plan(std::span<const SectionAtoms> sections) {
        std::vector<std::vector<Stretch>> stretches(sections.size());
        std::vector<agents::Mission> missions;
        std::size_t planned = 0;  // sections the commander is asked about, whole or in part
        for (std::size_t index = 0; index < sections.size(); ++index) {
            if (!sections[index].atoms.empty()) {
                const std::size_t before = missions.size();
                RR_TRY_ASSIGN(stretches[index], stretches_of(sections[index], index + 1, missions));
                if (missions.size() > before) {
                    ++planned;
                }
            }
        }
        if (!missions.empty()) {
            progress(std::format(
                "plan: the commander plans {} {}{}", planned, planned == 1 ? "section" : "sections",
                missions.size() > planned ? std::format(", in {} parts", missions.size()) : ""));
        }
        RR_TRY_ASSIGN(const std::vector<agents::Outcome> outcomes,
                      army_->squad(std::move(missions), context_->stop));
        knowledge::Plan plan;
        for (std::size_t index = 0; index < sections.size(); ++index) {
            plan.sections.push_back(
                sections[index].atoms.empty()
                    ? grouped_section(sections[index])
                    : assembled(sections[index], index + 1, stretches[index], outcomes, plan.duplicates));
        }
        return plan;
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

    // The stretches of `section`'s atoms (see Stretch), each the most the
    // commander holds from where the last one ended; their missions are
    // added to `missions`.
    [[nodiscard]] Result<std::vector<Stretch>> stretches_of(const SectionAtoms& section, std::size_t number,
                                                            std::vector<agents::Mission>& missions) const {
        std::vector<Stretch> stretches;
        std::size_t parts = 0;  // missions of the section so far
        for (std::size_t begin = 0; begin < section.atoms.size();) {
            RR_TRY_ASSIGN(Held held, most_held(section, number, parts + 1, begin));
            if (!held.mission.has_value()) {
                // Set aside, with any set aside just before: told in the order said.
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

    // The mission planning the most atoms of `section` from `begin` that the
    // commander holds, its `part`th, and where its atoms end; no mission
    // when it holds not even the atom at `begin`.
    struct Held {
        std::size_t end = 0;
        std::optional<agents::Mission> mission{};
    };

    [[nodiscard]] Result<Held> most_held(const SectionAtoms& section, std::size_t number, std::size_t part,
                                         std::size_t begin) const {
        const std::size_t count = section.atoms.size();
        const auto id = [&](std::size_t end) {
            return begin == 0 && end == count ? std::format("plan-{}", number)
                                              : std::format("plan-{}-{}", number, part);
        };
        RR_TRY_ASSIGN(agents::Mission whole, mission_of(id(count), section, number, begin, count));
        if (army_->holds(whole)) {
            return Held{.end = count, .mission = std::move(whole)};
        }
        // Bisect for the most atoms the commander holds: `held` it holds (or
        // is `begin`, none), `refused` it does not.
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

    // A mission to plan atoms [begin, end) of `section`, the `number`th.
    [[nodiscard]] Result<agents::Mission> mission_of(std::string id, const SectionAtoms& section,
                                                     std::size_t number, std::size_t begin,
                                                     std::size_t end) const {
        const std::span<const Atom* const> atoms = std::span{section.atoms}.subspan(begin, end - begin);
        std::string lines;
        for (const Atom* const atom : atoms) {
            lines += (lines.empty() ? "" : "\n") + atom_line(*atom, *transcript_);
        }
        const bool whole = begin == 0 && end == section.atoms.size();
        const json::Json order{
            {"number", number},
            {"section", section.section->title},
            {"start", format_timestamp(sentence(section.section->first).span.start())},
            {"end", format_timestamp(sentence(section.section->last).span.end())},
            {"section_gist", section.section->gist.value_or("")},
            {"part", whole
                         ? std::string{}
                         : std::format("Its atoms are too many to plan at once: plan {} here; the others are "
                                       "planned apart.",
                                       named(atoms))},
            {"atoms", std::move(lines)}};
        RR_TRY_ASSIGN(const prompt::Rendered rendered, context_->prompts->render(kPrompt, shared_, order));
        const std::int64_t seed = llm::derived_seed(context_->job->salt, PlanStage::kName, id, 0);
        return agents::Mission{
            .id = std::move(id),
            .kind = std::string{kPrompt},
            .prompt = prompt_.info->label(),
            .role = "commander",
            .request =
                llm::ChatRequest{
                    .messages = {llm::Message::system(rendered.system), llm::Message::user(rendered.user)},
                    .seed = seed,
                    .thinking = true,
                    .thinking_budget = llm::ThinkingBudget{.tokens = thinking_budget_, .closing = closing_}},
            .answer_tokens = answer_tokens(atoms.size()),
            .schema = prompt_.schema,
            .check = [asked = Asked{.atoms = {atoms.begin(), atoms.end()}, .range = named(atoms)}](
                         const json::Json& answer) { return answer_problems(answer, asked); }};
    }

    [[nodiscard]] const knowledge::Sentence& sentence(knowledge::SentenceId id) const {
        const knowledge::Sentence* found = transcript_->find(id);
        RR_ASSERT(found != nullptr, "the outline covers its transcript (read_input())");
        return *found;
    }

    // `section`'s plan, from the answers to its stretches' missions, in
    // order; what no answer planned is grouped, and said. Its duplicates
    // are added to `duplicates`.
    knowledge::SectionPlan assembled(const SectionAtoms& section, std::size_t number,
                                     std::span<const Stretch> stretches,
                                     std::span<const agents::Outcome> outcomes,
                                     std::vector<knowledge::Duplicate>& duplicates) {
        knowledge::SectionPlan plan{.first = section.section->first,
                                    .heading = section.section->title,
                                    .planned_by = knowledge::PlannedBy::grouping};
        bool headed = false;
        for (const Stretch& stretch : stretches) {
            const std::span<const Atom* const> atoms =
                std::span{section.atoms}.subspan(stretch.begin, stretch.end - stretch.begin);
            const agents::Outcome* const outcome =
                stretch.mission.has_value() ? &outcomes[*stretch.mission] : nullptr;
            if (outcome != nullptr && outcome->verdict == agents::Verdict::ceiling) {
                output_.reusable = false;  // a job allowed more would have planned it
            }
            if (outcome == nullptr || outcome->answers.empty()) {
                const bool whole = atoms.size() == section.atoms.size();
                output_.findings.push_back(std::format(
                    "section {}, \"{}\": {} told in the order said, a paragraph at a time: {}", number,
                    section.section->title,
                    whole ? std::string{"its atoms are"}
                          : named(atoms) + (atoms.size() == 1 ? " is" : " are"),
                    outcome == nullptr ? std::format("even alone, {} more than {} holds at once",
                                                     atoms.size() == 1 ? "it is" : "each is", model())
                                       : why_unanswered(*outcome, *army_, "plan it")));
                std::ranges::move(grouped(atoms), std::back_inserter(plan.paragraphs));
                continue;
            }
            Planned planned = planned_of(outcome->answers.front().value);
            if (!headed) {
                plan.heading = std::move(planned.heading);
                headed = true;
            }
            plan.planned_by = knowledge::PlannedBy::commander;
            std::ranges::move(planned.paragraphs, std::back_inserter(plan.paragraphs));
            std::ranges::move(planned.duplicates, std::back_inserter(duplicates));
        }
        return plan;
    }

    const StageContext* context_;
    const Transcript* transcript_;
    agents::Army* army_;
    StagePrompt prompt_;
    json::Json shared_;
    std::string closing_;      // the commander's words that end its thinking
    int thinking_budget_ = 0;  // a plan's (see the constructor)
    StageOutput output_;
};

// What the prompt's system part shows: the video, its gist, and the
// article's sections, the same for every section planned.
json::Json shared_of(const Source& source, const Transcript& transcript, const knowledge::Outline& outline) {
    std::string sections;
    std::size_t number = 0;
    for (const knowledge::Section& section : outline.sections) {
        const knowledge::Sentence* first = transcript.find(section.first);
        RR_ASSERT(first != nullptr, "the outline covers its transcript (read_input())");
        sections += std::format("{}{}. [{}] {}{}", sections.empty() ? "" : "\n", ++number,
                                format_timestamp(first->span.start()), section.title,
                                section.gist.has_value() ? ": " + *section.gist : "");
    }
    const json::Json video = video_shared(source.video);
    return json::Json{{"title", video.at("title")},
                      {"channel", video.at("channel")},
                      {"gist", outline.brief.gist.value_or("")},
                      {"sections", std::move(sections)}};
}

struct Read {
    Source source{};
    Transcript transcript{};
    knowledge::Outline outline{};
    knowledge::AtomLedger ledger{};
    reason::Tier tier{};
};

Result<Read> read_input(const StageContext& context) {
    RR_ASSERT(context.job != nullptr, "a stage runs for a job");
    Read read;
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
    const std::size_t said = read.transcript.sentences.size();
    for (const Atom& atom : read.ledger.atoms) {
        if (atom.sentences.back().number() > said) {
            return fail(Errc::internal,
                        std::format("the ledger's {} is said at {}, past the transcript's last "
                                    "sentence, S{}",
                                    atom.id, atom.sentences.back(), said));
        }
    }
    RR_TRY_ASSIGN(read.tier, job_tier(context));
    return read;
}

}  // namespace

Result<json::Json> PlanStage::fingerprint(const StageContext& context) const {
    RR_ASSERT(context.job != nullptr, "a stage runs for a job");
    RR_TRY_ASSIGN(const reason::Tier tier, job_tier(context));
    json::Json fingerprint{{"tier", tier.canonical(reason::Stage::plan)},
                           {"limits", json::Json{{"paragraph_atoms", kParagraphAtoms},
                                                 {"heading_words", kHeadingWords},
                                                 {"purpose_words", kPurposeWords},
                                                 {"grouped_atoms", kGroupedAtoms}}},
                           {"models", nullptr}};
    if (tier.plan.mode == reason::PlanMode::group) {
        return fingerprint;
    }
    if (!context.army) {
        return fail(Errc::needs_user,
                    std::format("planning at the tier '{}' needs models, and no pod is configured: rreport "
                                "doctor says how to set one up",
                                tier.name));
    }
    RR_ASSERT(context.prompts != nullptr, "a stage that asks models is given the prompts");
    RR_TRY_ASSIGN(json::Json prompts, prompts_fingerprint(*context.prompts, {kPrompt}));
    RR_TRY_ASSIGN(agents::Army* const army, context.army());
    const agents::Engine* commander = engine_serving(*army, "commander");
    fingerprint["models"] = json::Json{{"prompts", std::move(prompts)},
                                       {"army", army_fingerprint(*army)},
                                       {"holds", commander == nullptr ? 0 : commander->ctx_per_slot}};
    return fingerprint;
}

Result<StageOutput> PlanStage::run(const StageContext& context) const {
    RR_TRY_ASSIGN(const Read read, read_input(context));
    const std::vector<SectionAtoms> sections = sections_of(read.outline, read.ledger);
    if (read.tier.plan.mode == reason::PlanMode::group) {
        if (context.progress) {
            context.progress("plan: the atoms are told in the order said, a paragraph at a time");
        }
        knowledge::Plan plan;
        plan.sections.reserve(sections.size());
        for (const SectionAtoms& section : sections) {
            plan.sections.push_back(grouped_section(section));
        }
        return StageOutput{.artifact = knowledge::to_json(plan)};
    }
    RR_TRY_ASSIGN(agents::Army* const army, context.army());
    RR_TRY_ASSIGN(StagePrompt prompt, stage_prompt(*context.prompts, kPrompt));
    Planner planner{context, read.transcript,   read.tier.plan,
                    *army,   std::move(prompt), shared_of(read.source, read.transcript, read.outline)};
    RR_TRY_ASSIGN(const knowledge::Plan plan, planner.plan(sections));
    StageOutput output = std::move(planner.output());
    output.artifact = knowledge::to_json(plan);
    return output;
}

}  // namespace rr::pipeline

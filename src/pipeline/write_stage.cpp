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
#include <rr/pipeline/drafting.hpp>
#include <rr/pipeline/judging.hpp>
#include <rr/pipeline/outline_stage.hpp>
#include <rr/pipeline/plan_stage.hpp>
#include <rr/pipeline/source.hpp>
#include <rr/pipeline/transcript_stage.hpp>
#include <rr/reason/pairwise.hpp>
#include <rr/reason/select.hpp>
#include <rr/reason/tier.hpp>
#include <rr/text/align.hpp>

namespace rr::pipeline {

namespace {

using knowledge::Atom;
using knowledge::AtomId;
using knowledge::Transcript;

constexpr std::string_view kPrompt = "write";
constexpr std::string_view kFusePrompt = "fuse";
constexpr std::string_view kJudgePrompt = "judge";
constexpr std::string_view kFlowPrompt = "flow";
constexpr int kFlowTokens = 512;  // a comparison's reasons, and a word

// A fusion is an extra, never a rescue: it has the repairs a draft has and no
// rung beyond, so one that fails leaves the drafts as they were. Above all it
// is never handed to another family: what Self-MoA fuses is the strongest
// model's own drafts, and a weaker model's would lower their quality.
constexpr agents::Escalation kFuseEscalation{.thinking = false, .commander = false, .other_family = false};

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

// The room an answer telling `paragraphs` takes (prose_tokens()).
int answer_tokens(std::span<const ToWrite> paragraphs) {
    std::int64_t bytes = 0;
    for (const ToWrite& paragraph : paragraphs) {
        for (const Atom* const atom : paragraph.atoms) {
            bytes += atom_bytes(*atom);
        }
    }
    return prose_tokens(bytes, paragraphs.size());
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

// --- The judge's part ------------------------------------------------------------------

// The drafts of a mission that passed, best first by Ledger B (fewest
// warnings; on a tie, their fusion first, then the first drawn), and where
// the judge's missions about the first `judged` of them begin in the judges'
// squad: a reading of each, then each pair compared in both orders.
struct Drafts {
    std::string written{};  // the mission that drew them: the fusion's and the judge's ids extend its
    std::vector<const agents::Answer*> ranked{};
    std::vector<std::size_t> warnings{};  // of each, ranked
    std::optional<std::size_t> fused{};   // where their fusion is in `ranked`, if one passed
    std::size_t judged = 0;               // none: picked by Ledger B alone
    std::size_t first = 0;
};

// Where, in a judges' squad, the comparisons of drafts `a` and `b` (a < b)
// are: (a, b) shown in that order, then (b, a) right after.
std::size_t flow_index(const Drafts& drafts, std::size_t a, std::size_t b) {
    std::size_t pairs = 0;  // before (a, b), in order: (0, 1), (0, 2), …, (1, 2), …
    for (std::size_t first = 0; first < a; ++first) {
        pairs += drafts.judged - first - 1;
    }
    pairs += b - a - 1;
    return drafts.first + drafts.judged + (2 * pairs);
}

// Paragraphs [begin, end) of `section`.
std::span<const ToWrite> paragraphs_in(const SectionToWrite& section, const Stretch& stretch) {
    return std::span{section.paragraphs}.subspan(stretch.begin, stretch.end - stretch.begin);
}

// What the commander makes of the sections: each written, in parts where it
// does not hold one whole, or told as its atoms say it where no draft
// passed. What it could not do is noted in output(), as findings.
class Writer {
public:
    // The prompts a writer asks with: its own, the fusion's where the tier
    // fuses, and the judge's two.
    struct Prompts {
        StagePrompt write{};
        StagePrompt fuse{};
        StagePrompt judge{};
        StagePrompt flow{};
    };

    Writer(const StageContext& context, const Transcript& transcript, const knowledge::AtomLedger& ledger,
           const reason::WriteCompute& compute, agents::Army& army, Prompts prompts, json::Json shared)
        : context_(&context)
        , transcript_(&transcript)
        , ledger_(&ledger)
        , compute_(compute)
        , army_(&army)
        , prompts_(std::move(prompts))
        , shared_(std::move(shared))
        , judge_shared_(json::Json{{"title", shared_.at("title")}, {"channel", shared_.at("channel")}}) {
        // A tier that writes thinking thinks with the commander's own budget,
        // within half of what it holds (the outline's rule).
        const agents::Engine* commander = engine_serving(army, "commander");
        if (commander != nullptr && commander->thinking.thinking_budget.has_value()) {
            budget_ = commander->thinking.thinking_budget;
            budget_->tokens = std::min(budget_->tokens, commander->ctx_per_slot / 2);
        }
        // The judge is the infantry, when it is of another family than the
        // commander that writes.
        const agents::Engine* judge = engine_serving(army, "infantry");
        if (commander != nullptr && judge != nullptr && judge->family != commander->family) {
            judge_family_ = judge->family;
        }
    }

    // Each section's paragraphs, in order: none for a section with none.
    Result<std::vector<std::vector<std::string>>> write(std::span<const SectionToWrite> sections) {
        std::vector<agents::Mission> missions;
        RR_TRY_ASSIGN(const std::vector<std::vector<Stretch>> stretches, stretches_all(sections, missions));
        RR_TRY_ASSIGN(const std::vector<agents::Outcome> outcomes,
                      army_->squad(std::move(missions), context_->stop));
        // The drafts that passed, ranked by Ledger B; where the tier fuses,
        // one draft more of each stretch's; the best of them judged, where a
        // judge of another family can read them.
        std::vector<std::vector<Drafts>> drafts = ranked_all(sections, stretches, outcomes);
        std::vector<agents::Outcome> fused;  // the fusions: their drafts point into it
        if (compute_.fuse) {
            RR_TRY(fuse(sections, stretches, drafts, fused));
        }
        RR_TRY_ASSIGN(std::vector<agents::Mission> judges, judges_all(sections, stretches, drafts));
        RR_TRY_ASSIGN(const std::vector<agents::Outcome> verdicts,
                      army_->squad(std::move(judges), context_->stop));
        std::vector<std::vector<std::string>> paragraphs(sections.size());
        for (std::size_t index = 0; index < sections.size(); ++index) {
            for (std::size_t part = 0; part < stretches[index].size(); ++part) {
                std::ranges::move(assembled(sections[index], index + 1, stretches[index][part], outcomes,
                                            drafts[index][part], verdicts),
                                  std::back_inserter(paragraphs[index]));
            }
        }
        if (fusions_ > 0) {
            progress(std::format("write: {} of {} fused {} kept", fusions_kept_, fusions_,
                                 fusions_ == 1 ? "draft" : "drafts"));
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

    // Each section's stretches, their missions added to `missions`; what the
    // commander is asked for is said.
    [[nodiscard]] Result<std::vector<std::vector<Stretch>>> stretches_all(
        std::span<const SectionToWrite> sections, std::vector<agents::Mission>& missions) {
        std::vector<std::vector<Stretch>> stretches(sections.size());
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
        return stretches;
    }

    // Each stretch's drafts that passed, ranked by Ledger B.
    [[nodiscard]] std::vector<std::vector<Drafts>> ranked_all(
        std::span<const SectionToWrite> sections, std::span<const std::vector<Stretch>> stretches,
        std::span<const agents::Outcome> outcomes) const {
        std::vector<std::vector<Drafts>> drafts(sections.size());
        for (std::size_t index = 0; index < sections.size(); ++index) {
            drafts[index].reserve(stretches[index].size());
            for (const Stretch& stretch : stretches[index]) {
                drafts[index].push_back(ranked_of(sections[index], stretch, outcomes));
            }
        }
        return drafts;
    }

    // The judge's missions about each stretch's best drafts, where a judge
    // of another family can read them (judged_by()); what it is asked is said.
    [[nodiscard]] Result<std::vector<agents::Mission>> judges_all(
        std::span<const SectionToWrite> sections, std::span<const std::vector<Stretch>> stretches,
        std::span<std::vector<Drafts>> drafts) {
        std::vector<agents::Mission> judges;
        std::size_t judged = 0;  // sections whose drafts are judged
        for (std::size_t index = 0; index < sections.size(); ++index) {
            bool any = false;
            for (std::size_t part = 0; part < stretches[index].size(); ++part) {
                Drafts& ranked = drafts[index][part];
                RR_TRY(judged_by(sections[index], index + 1, stretches[index][part], ranked, judges));
                any = any || ranked.judged > 0;
            }
            if (any) {
                ++judged;
            }
        }
        if (!judges.empty()) {
            progress(std::format("write: the infantry judges the best {} drafts of {} {}", compute_.judge_top,
                                 judged, judged == 1 ? "section" : "sections"));
        }
        return judges;
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

    // What a mission writing paragraphs [begin, end) of `section`, the
    // `number`th, asks: the order its prompt is rendered with, what its
    // answer is held to, and the room the answer is given. A draft and the
    // fusion of drafts ask the same.
    struct Brief {
        json::Json order{};
        Asked asked{};
        int answer_tokens = 0;
    };

    [[nodiscard]] Brief brief_of(const SectionToWrite& section, std::size_t number, std::size_t begin,
                                 std::size_t end) const {
        const std::span<const ToWrite> paragraphs = std::span{section.paragraphs}.subspan(begin, end - begin);
        Brief brief{.asked = Asked{.first = begin + 1}, .answer_tokens = answer_tokens(paragraphs)};
        std::string lines;
        for (std::size_t index = 0; index < paragraphs.size(); ++index) {
            const ToWrite& paragraph = paragraphs[index];
            lines += std::format("{}Paragraph {}: {}", lines.empty() ? "" : "\n", begin + index + 1,
                                 paragraph.purpose.value_or("Tell its atoms in the order given."));
            brief.asked.paragraphs.emplace_back();
            for (const Atom* const atom : paragraph.atoms) {
                lines += "\n" + atom_entry(*atom, *transcript_);
                brief.asked.paragraphs.back().push_back(atom->id);
                brief.asked.atoms.push_back(atom->id);
            }
        }
        const bool whole = begin == 0 && end == section.paragraphs.size();
        brief.order = json::Json{
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
        return brief;
    }

    // The check of an answer writing what `asked` asks (draft_problems()).
    [[nodiscard]] llm::AnswerCheck check_of(Asked asked) const {
        return
            [transcript = transcript_, ledger = ledger_, asked = std::move(asked)](const json::Json& answer) {
                return draft_problems(answer.get<std::string>(), *transcript, *ledger, asked);
            };
    }

    // The commander's request for mission `id`'s draft: the prompt as
    // rendered, its seed, and thinking as the tier writes, within the
    // commander's budget.
    [[nodiscard]] llm::ChatRequest draft_request(const prompt::Rendered& rendered,
                                                 std::string_view id) const {
        llm::ChatRequest request{
            .messages = {llm::Message::system(rendered.system), llm::Message::user(rendered.user)},
            .seed = llm::derived_seed(context_->job->salt, WriteStage::kName, id, 0),
            .thinking = compute_.thinking};
        if (compute_.thinking) {
            request.thinking_budget = budget_;
        }
        return request;
    }

    // A mission to write paragraphs [begin, end) of `section`, the `number`th.
    [[nodiscard]] Result<agents::Mission> mission_of(std::string id, const SectionToWrite& section,
                                                     std::size_t number, std::size_t begin,
                                                     std::size_t end) const {
        Brief brief = brief_of(section, number, begin, end);
        RR_TRY_ASSIGN(const prompt::Rendered rendered,
                      context_->prompts->render(kPrompt, shared_, brief.order));
        llm::ChatRequest request = draft_request(rendered, id);
        return agents::Mission{.id = std::move(id),
                               .kind = std::string{kPrompt},
                               .prompt = prompts_.write.info->label(),
                               .role = "commander",
                               .request = std::move(request),
                               .answer_tokens = brief.answer_tokens,
                               .check = check_of(std::move(brief.asked)),
                               .repairs = compute_.repair_rounds,
                               .samples = compute_.drafts};
    }

    // A mission fusing `drafts` of `stretch`, best first, into one
    // (prompts/fuse): the commander is shown the paragraphs as their writer
    // was, and each draft, and writes the one that takes the best of them,
    // held to what a draft is held to.
    [[nodiscard]] Result<agents::Mission> fusion_of(std::string id, const SectionToWrite& section,
                                                    std::size_t number, const Stretch& stretch,
                                                    std::span<const agents::Answer* const> drafts) const {
        Brief brief = brief_of(section, number, stretch.begin, stretch.end);
        std::string shown;
        for (std::size_t index = 0; index < drafts.size(); ++index) {
            shown += std::format("{}Draft {}:\n<draft>\n{}\n</draft>", index == 0 ? "" : "\n", index + 1,
                                 drafts[index]->value.get<std::string>());
        }
        brief.order["count"] = drafts.size();
        brief.order["drafts"] = std::move(shown);
        RR_TRY_ASSIGN(const prompt::Rendered rendered,
                      context_->prompts->render(kFusePrompt, shared_, brief.order));
        llm::ChatRequest request = draft_request(rendered, id);
        return agents::Mission{.id = std::move(id),
                               .kind = std::string{kFusePrompt},
                               .prompt = prompts_.fuse.info->label(),
                               .role = "commander",
                               .request = std::move(request),
                               .answer_tokens = brief.answer_tokens,
                               .check = check_of(std::move(brief.asked)),
                               .repairs = compute_.repair_rounds,
                               .samples = 1,
                               .escalation = kFuseEscalation};
    }

    [[nodiscard]] const knowledge::Sentence& sentence(knowledge::SentenceId id) const {
        const knowledge::Sentence* found = transcript_->find(id);
        RR_ASSERT(found != nullptr, "the outline covers its transcript (read_input())");
        return *found;
    }

    // The paragraphs of `stretch`: the draft kept of `drafts`, or, where no
    // draft passed, told as said, and said.
    std::vector<std::string> assembled(const SectionToWrite& section, std::size_t number,
                                       const Stretch& stretch, std::span<const agents::Outcome> outcomes,
                                       const Drafts& drafts, std::span<const agents::Outcome> verdicts) {
        const std::span<const ToWrite> paragraphs = paragraphs_in(section, stretch);
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
        const std::size_t kept = picked(section, number, drafts, verdicts);
        if (drafts.fused == kept) {
            ++fusions_kept_;
        }
        return paragraphs_of(drafts.ranked[kept]->value.get<std::string>());
    }

    // What Ledger B holds a draft of `stretch` to: its atoms.
    [[nodiscard]] static Asked atoms_of(const SectionToWrite& section, const Stretch& stretch) {
        Asked asked;
        for (const ToWrite& paragraph : paragraphs_in(section, stretch)) {
            for (const Atom* const atom : paragraph.atoms) {
                asked.atoms.push_back(atom->id);
            }
        }
        return asked;
    }

    // `stretch`'s drafts that passed, ranked by Ledger B (see Drafts).
    [[nodiscard]] Drafts ranked_of(const SectionToWrite& section, const Stretch& stretch,
                                   std::span<const agents::Outcome> outcomes) const {
        Drafts drafts;
        if (!stretch.mission.has_value() || outcomes[*stretch.mission].answers.empty()) {
            return drafts;
        }
        const agents::Outcome& outcome = outcomes[*stretch.mission];
        drafts.written = outcome.mission;
        const Asked asked = atoms_of(section, stretch);
        std::vector<std::pair<std::size_t, const agents::Answer*>> scored;
        scored.reserve(outcome.answers.size());
        for (const agents::Answer& answer : outcome.answers) {
            scored.emplace_back(warnings_of(answer.value.get<std::string>(), *transcript_, *ledger_, asked),
                                &answer);
        }
        std::ranges::stable_sort(scored, {}, &std::pair<std::size_t, const agents::Answer*>::first);
        drafts.ranked.reserve(scored.size() + 1);  // and a fusion of them
        drafts.warnings.reserve(scored.size() + 1);
        for (const auto& [warnings, answer] : scored) {
            drafts.ranked.push_back(answer);
            drafts.warnings.push_back(warnings);
        }
        return drafts;
    }

    // Each stretch's drafts that passed, two or more, fused into one more by
    // the commander (Self-MoA), its missions' outcomes kept in `fused`, which
    // the fused drafts that pass point into.
    [[nodiscard]] Result<void> fuse(std::span<const SectionToWrite> sections,
                                    std::span<const std::vector<Stretch>> stretches,
                                    std::span<std::vector<Drafts>> drafts,
                                    std::vector<agents::Outcome>& fused) {
        std::vector<std::pair<std::size_t, std::size_t>> fusing;  // each mission's section and part
        std::vector<agents::Mission> missions;
        std::size_t sections_fused = 0;
        for (std::size_t index = 0; index < sections.size(); ++index) {
            const std::size_t before = missions.size();
            for (std::size_t part = 0; part < stretches[index].size(); ++part) {
                RR_TRY_ASSIGN(
                    std::optional<agents::Mission> mission,
                    fusion_held(sections[index], index + 1, stretches[index][part], drafts[index][part]));
                if (mission.has_value()) {
                    fusing.emplace_back(index, part);
                    missions.push_back(std::move(*mission));
                }
            }
            if (missions.size() > before) {
                ++sections_fused;
            }
        }
        if (missions.empty()) {
            return {};
        }
        progress(std::format("write: the commander fuses the drafts of {} {}", sections_fused,
                             sections_fused == 1 ? "section" : "sections"));
        RR_TRY_ASSIGN(fused, army_->squad(std::move(missions), context_->stop));
        for (std::size_t mission = 0; mission < fused.size(); ++mission) {
            const auto [index, part] = fusing[mission];
            joined(sections[index], index + 1, stretches[index][part], fused[mission], drafts[index][part]);
        }
        return {};
    }

    // The mission fusing `drafts`, those of `stretch` that passed, best
    // first, as many as the commander holds: the worst are left out first.
    // None where fewer than two passed, or where the commander holds not
    // even two, which is said.
    [[nodiscard]] Result<std::optional<agents::Mission>> fusion_held(const SectionToWrite& section,
                                                                     std::size_t number,
                                                                     const Stretch& stretch,
                                                                     const Drafts& drafts) {
        if (drafts.ranked.size() < 2) {
            return std::nullopt;  // nothing to fuse
        }
        const std::string id = std::format(
            "fuse-{}", std::string_view{drafts.written}.substr(std::string_view{"write-"}.size()));
        for (std::size_t shown = drafts.ranked.size(); shown >= 2; --shown) {
            RR_TRY_ASSIGN(agents::Mission mission,
                          fusion_of(id, section, number, stretch, std::span{drafts.ranked}.first(shown)));
            if (army_->holds(mission)) {
                return mission;
            }
        }
        const std::string unheld = std::format(
            "section {}, \"{}\": its drafts are not fused: two of them are more than {} holds at once",
            number, section.plan->heading, model());
        if (std::ranges::find(output_.findings, unheld) == output_.findings.end()) {
            output_.findings.push_back(unheld);
        }
        return std::nullopt;
    }

    // The fusion of `drafts`, those of `stretch`: where its draft passed, it
    // joins them, first among those with as few warnings, since it was
    // written from them all; where none did, they stand as drawn, and it is
    // said.
    void joined(const SectionToWrite& section, std::size_t number, const Stretch& stretch,
                const agents::Outcome& outcome, Drafts& drafts) {
        noted(outcome);
        if (outcome.answers.empty()) {
            output_.findings.push_back(std::format("section {}, \"{}\": its drafts could not be fused: {}",
                                                   number, section.plan->heading,
                                                   why_unanswered(outcome, *army_, "fuse them")));
            return;
        }
        const agents::Answer& fusion = outcome.answers.front();
        const std::size_t warnings =
            warnings_of(fusion.value.get<std::string>(), *transcript_, *ledger_, atoms_of(section, stretch));
        const auto at = std::ranges::lower_bound(drafts.warnings, warnings);
        const std::ptrdiff_t place = at - drafts.warnings.begin();
        drafts.warnings.insert(at, warnings);
        drafts.ranked.insert(drafts.ranked.begin() + place, &fusion);
        drafts.fused = static_cast<std::size_t>(place);
        ++fusions_;
    }

    // The first `judge_top` of `drafts` judged, where a judge of another
    // family can read them, and holds them: its missions added to `judges`.
    // Drafts to judge, two or more, are the commander's: an escalation draws
    // one answer (rr/agents/army.hpp), and a fusion is never escalated. So a
    // judge of another family than the commander's is of another family
    // than every draft it reads.
    [[nodiscard]] Result<void> judged_by(const SectionToWrite& section, std::size_t number,
                                         const Stretch& stretch, Drafts& drafts,
                                         std::vector<agents::Mission>& judges) {
        const std::size_t judged =
            std::min(static_cast<std::size_t>(std::max(compute_.judge_top, 0)), drafts.ranked.size());
        if (judged < 2) {
            return {};  // nothing to choose between
        }
        if (!judge_family_.has_value()) {
            if (!unjudged_said_) {
                output_.findings.emplace_back(
                    "the drafts are picked by Ledger B alone: "
                    "the pod has no judge of another family than the writer's");
                unjudged_said_ = true;
            }
            return {};
        }
        drafts.judged = judged;
        std::vector<agents::Mission> missions;
        RR_TRY(judge_missions(section, number, stretch, drafts.written, drafts, missions));
        if (!std::ranges::all_of(missions,
                                 [this](const agents::Mission& mission) { return army_->holds(mission); })) {
            // squad() would refuse the lot: the drafts are picked without it.
            const std::string unheld = std::format(
                "section {}, \"{}\": the drafts are picked by Ledger B alone: they are more than the "
                "judge holds at once",
                number, section.plan->heading);
            if (std::ranges::find(output_.findings, unheld) == output_.findings.end()) {
                output_.findings.push_back(unheld);
            }
            drafts.judged = 0;
            return {};
        }
        drafts.first = judges.size();
        std::ranges::move(missions, std::back_inserter(judges));
        return {};
    }

    // The judge's missions about the first `drafts.judged` of `drafts`: a
    // reading of each, then each pair compared in both orders; their ids
    // after the writing mission's (`write-2-1` gives `judge-2-1-draft-1`).
    [[nodiscard]] Result<void> judge_missions(const SectionToWrite& section, std::size_t number,
                                              const Stretch& stretch, std::string_view written,
                                              const Drafts& drafts,
                                              std::vector<agents::Mission>& judges) const {
        const std::string_view id = written.substr(std::string_view{"write-"}.size());
        for (std::size_t draft = 0; draft < drafts.judged; ++draft) {
            RR_TRY_ASSIGN(agents::Mission judge,
                          judge_mission(std::format("judge-{}-draft-{}", id, draft + 1), section, number,
                                        stretch, drafts.ranked[draft]->value.get<std::string>()));
            judges.push_back(std::move(judge));
        }
        for (std::size_t a = 0; a < drafts.judged; ++a) {
            for (std::size_t b = a + 1; b < drafts.judged; ++b) {
                for (const auto& [one, other] : {std::pair{a, b}, std::pair{b, a}}) {
                    RR_TRY_ASSIGN(agents::Mission flow,
                                  flow_mission(std::format("flow-{}-drafts-{}-{}", id, one + 1, other + 1),
                                               section, number, drafts.ranked[one]->value.get<std::string>(),
                                               drafts.ranked[other]->value.get<std::string>()));
                    judges.push_back(std::move(flow));
                }
            }
        }
        return {};
    }

    // A mission reading `draft`'s sentences against the atoms of `stretch`.
    [[nodiscard]] Result<agents::Mission> judge_mission(std::string id, const SectionToWrite& section,
                                                        std::size_t number, const Stretch& stretch,
                                                        std::string_view draft) const {
        std::string atoms;
        for (const ToWrite& paragraph : paragraphs_in(section, stretch)) {
            for (const Atom* const atom : paragraph.atoms) {
                atoms += (atoms.empty() ? "" : "\n") + atom_entry(*atom, *transcript_);
            }
        }
        return pipeline::judge_mission(
            *context_, WriteStage::kName, prompts_.judge, judge_shared_,
            ToJudge{.id = std::move(id),
                    .what = std::format("section {}, \"{}\"", number, section.plan->heading),
                    .atoms = std::move(atoms),
                    .draft = std::string{draft}});
    }

    // A mission comparing how `first` and `second`, shown in that order, read.
    [[nodiscard]] Result<agents::Mission> flow_mission(std::string id, const SectionToWrite& section,
                                                       std::size_t number, std::string_view first,
                                                       std::string_view second) const {
        const json::Json order{{"number", number},
                               {"heading", section.plan->heading},
                               {"first", plain(first)},
                               {"second", plain(second)}};
        RR_TRY_ASSIGN(const prompt::Rendered rendered,
                      context_->prompts->render(kFlowPrompt, judge_shared_, order));
        const std::int64_t seed = llm::derived_seed(context_->job->salt, WriteStage::kName, id, 0);
        return agents::Mission{.id = std::move(id),
                               .kind = std::string{kFlowPrompt},
                               .prompt = prompts_.flow.info->label(),
                               .role = "infantry",
                               .request = llm::ChatRequest{.messages = {llm::Message::system(rendered.system),
                                                                        llm::Message::user(rendered.user)},
                                                           .seed = seed,
                                                           .thinking = false},
                               .answer_tokens = kFlowTokens,
                               .schema = prompts_.flow.schema,
                               .escalation = kJudgeEscalation};
    }

    // What the judge read in each of the first `drafts.judged`; none when a
    // reading is missing, which is said.
    std::optional<std::vector<std::vector<Judged>>> readings_of(const Drafts& drafts,
                                                                std::span<const agents::Outcome> verdicts,
                                                                std::string_view where) {
        std::vector<std::vector<Judged>> readings;
        readings.reserve(drafts.judged);
        for (std::size_t draft = 0; draft < drafts.judged; ++draft) {
            const agents::Outcome& outcome = verdicts[drafts.first + draft];
            noted(outcome);
            if (outcome.answers.empty()) {
                output_.findings.push_back(std::format("{}: the judge could not read its drafts: {}", where,
                                                       why_unanswered(outcome, *army_, "judge them")));
                return std::nullopt;
            }
            readings.push_back(judgment_of(outcome.answers.front().value));
        }
        return readings;
    }

    // How many comparisons each of the first `drafts.judged` lost, both
    // orders agreeing; none when a comparison is missing, which is said.
    std::optional<std::vector<std::size_t>> losses_of(const Drafts& drafts,
                                                      std::span<const agents::Outcome> verdicts,
                                                      std::string_view where) {
        std::vector<std::size_t> lost(drafts.judged, 0);
        for (std::size_t a = 0; a < drafts.judged; ++a) {
            for (std::size_t b = a + 1; b < drafts.judged; ++b) {
                const std::size_t index = flow_index(drafts, a, b);
                for (const agents::Outcome& outcome : verdicts.subspan(index, 2)) {
                    noted(outcome);
                    if (outcome.answers.empty()) {
                        output_.findings.push_back(
                            std::format("{}: the judge could not compare how its drafts read: {}", where,
                                        why_unanswered(outcome, *army_, "compare them")));
                        return std::nullopt;
                    }
                }
                const reason::Winner winner =
                    reason::swapped(preference_of(verdicts[index].answers.front().value),
                                    preference_of(verdicts[index + 1].answers.front().value));
                if (winner == reason::Winner::a) {
                    ++lost[b];
                } else if (winner == reason::Winner::b) {
                    ++lost[a];
                }
            }
        }
        return lost;
    }

    // Of `drafts`, the one kept: where the judge read them, the fewest issues
    // it read, then the fewest warnings, then the fewest comparisons lost
    // (reason::select_lexicographic()); else the first, Ledger B's best. What
    // the judge reads in the draft kept is said, as its reading.
    std::size_t picked(const SectionToWrite& section, std::size_t number, const Drafts& drafts,
                       std::span<const agents::Outcome> verdicts) {
        if (drafts.judged == 0) {
            return 0;
        }
        const std::string where = std::format("section {}, \"{}\"", number, section.plan->heading);
        const std::optional<std::vector<std::vector<Judged>>> readings = readings_of(drafts, verdicts, where);
        const std::optional<std::vector<std::size_t>> lost = losses_of(drafts, verdicts, where);
        std::vector<std::vector<double>> keys;
        keys.reserve(drafts.judged);
        for (std::size_t draft = 0; draft < drafts.judged; ++draft) {
            const std::size_t issues = readings.has_value() ? issues_in((*readings)[draft]) : 0;
            keys.push_back({static_cast<double>(issues), static_cast<double>(drafts.warnings[draft]),
                            lost.has_value() ? static_cast<double>((*lost)[draft]) : 0.0});
        }
        const std::size_t kept = reason::select_lexicographic(keys).value_or(0);
        if (readings.has_value()) {
            published(where, drafts.ranked[kept]->value.get<std::string>(), (*readings)[kept]);
        }
        return kept;
    }

    // What the judge read in `draft`, the one kept, said as its readings.
    void published(std::string_view where, std::string_view draft, std::span<const Judged> reading) {
        const std::vector<std::string> sentences = sentences_of(draft);
        for (const Judged& sentence : reading) {
            if (sentence.issue != Issue::none) {
                RR_ASSERT(sentence.sentence >= 1, "a reading numbers sentences from 1 (judgment_problems())");
                RR_ASSERT(sentence.sentence <= sentences.size(),
                          "a reading's sentences are its draft's (judgment_problems())");
                output_.findings.push_back(std::format(
                    "{}: the judge reads \"{}\" as {}: {}", where, shown(sentences[sentence.sentence - 1]),
                    reading_of(sentence.issue), sentence.reading.value_or("it gives no reading")));
            }
        }
    }

    // A judge's ceiling makes the result this job's alone, as a writer's does.
    void noted(const agents::Outcome& outcome) {
        if (outcome.verdict == agents::Verdict::ceiling) {
            output_.reusable = false;
        }
    }

    const StageContext* context_;
    const Transcript* transcript_;
    const knowledge::AtomLedger* ledger_;
    reason::WriteCompute compute_;
    agents::Army* army_;
    Prompts prompts_;
    json::Json shared_;
    json::Json judge_shared_;                    // the video, as the judge's prompts show it
    std::optional<llm::ThinkingBudget> budget_;  // a tier that writes thinking: the commander's
    std::optional<std::string> judge_family_;    // none: no judge apart from the writer
    bool unjudged_said_ = false;
    std::size_t fusions_ = 0;       // fused drafts that passed
    std::size_t fusions_kept_ = 0;  // of them, the drafts kept
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
    RR_TRY_ASSIGN(json::Json prompts,
                  prompts_fingerprint(*context.prompts, {kPrompt, kJudgePrompt, kFlowPrompt}));
    if (tier.write.fuse) {
        // Only a tier that fuses asks the fusion's prompt: its text keys no other.
        RR_TRY_ASSIGN(const json::Json fusion, prompts_fingerprint(*context.prompts, {kFusePrompt}));
        prompts.update(fusion);
    }
    RR_TRY_ASSIGN(agents::Army* const army, context.army());
    const agents::Engine* commander = engine_serving(*army, "commander");
    json::Json key{{"tier", tier.canonical(reason::Stage::write)},
                   {"prompts", std::move(prompts)},
                   {"army", army_fingerprint(*army)},
                   {"holds", commander == nullptr ? 0 : commander->ctx_per_slot}};
    if (tier.write.judge_top >= 2) {
        // Whether the judge holds a section's drafts decides whether they are judged.
        const agents::Engine* judge = engine_serving(*army, "infantry");
        key["judge_holds"] = judge == nullptr ? 0 : judge->ctx_per_slot;
    }
    return key;
}

Result<StageOutput> WriteStage::run(const StageContext& context) const {
    RR_TRY_ASSIGN(const Input read, read_input(context));
    RR_TRY_ASSIGN(agents::Army* const army, context.army());
    Writer::Prompts prompts;
    RR_TRY_ASSIGN(prompts.write, stage_prompt(*context.prompts, kPrompt));
    RR_TRY_ASSIGN(prompts.judge, stage_prompt(*context.prompts, kJudgePrompt));
    RR_TRY_ASSIGN(prompts.flow, stage_prompt(*context.prompts, kFlowPrompt));
    if (read.tier.write.fuse) {
        RR_TRY_ASSIGN(prompts.fuse, stage_prompt(*context.prompts, kFusePrompt));
    }
    const std::vector<SectionToWrite> sections = sections_of(read);
    Writer writer{context,
                  read.transcript,
                  read.ledger,
                  read.tier.write,
                  *army,
                  std::move(prompts),
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

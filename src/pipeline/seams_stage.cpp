// The stage that smooths the article's seams (rr/pipeline/seams_stage.hpp).
#include <rr/pipeline/seams_stage.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <rr/agents/mission.hpp>
#include <rr/core/assert.hpp>
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
#include <rr/pipeline/write_stage.hpp>
#include <rr/reason/tier.hpp>
#include <rr/text/span.hpp>

namespace rr::pipeline {

namespace {

using knowledge::AtomId;
using knowledge::Transcript;

constexpr std::string_view kPrompt = "seam";
constexpr std::string_view kJudgePrompt = "judge";
// A rewrite that fails its checks is sent back once: a seam that stays as
// written costs the article nothing.
constexpr int kRepairs = 1;
// A rewrite stays the commander's: after a refusal, no other family writes
// it, so the judge, of another family than the commander, never reads a
// rewrite of its own kind.
constexpr agents::Escalation kSeamEscalation{.thinking = true, .commander = true, .other_family = false};

struct Input {
    Source source{};
    Transcript transcript{};
    knowledge::AtomLedger ledger{};
    knowledge::Plan plan{};
    std::string article{};
    reason::Tier tier{};
};

Result<Input> read_input(const StageContext& context) {
    RR_ASSERT(context.job != nullptr, "a stage runs for a job");
    Input read;
    RR_TRY_ASSIGN(read.source, source_from_json(context.inputs.at(std::string{SourceStage::kName})));
    RR_TRY_ASSIGN(read.transcript,
                  knowledge::transcript_from_json(context.inputs.at(std::string{TranscriptStage::kName})));
    RR_TRY_ASSIGN(const knowledge::Outline outline,
                  knowledge::outline_from_json(context.inputs.at(std::string{OutlineStage::kName})));
    RR_TRY_ASSIGN(read.ledger,
                  knowledge::atom_ledger_from_json(context.inputs.at(std::string{AtomizeStage::kName})));
    RR_TRY_ASSIGN(read.plan, knowledge::plan_from_json(context.inputs.at(std::string{PlanStage::kName})));
    if (const std::vector<std::string> problems = knowledge::plan_problems(read.plan, read.ledger, outline);
        !problems.empty()) {
        return fail(Errc::internal, std::format("the plan does not hold its ledger: {}", problems.front()));
    }
    RR_TRY_ASSIGN(const knowledge::Article article,
                  knowledge::article_from_json(context.inputs.at(std::string{WriteStage::kName})));
    read.article = article.markdown;
    RR_TRY_ASSIGN(read.tier, job_tier(context));
    return read;
}

// --- Where the seams are ----------------------------------------------------------------

// The blocks of the article that tell one section of the plan, in order: a
// block belongs to the section of the first atom it cites.
struct Run {
    std::size_t section = 0;  // in the plan, from 0
    std::vector<const knowledge::Block*> blocks{};
};

// The section of the plan, by index, that tells each atom it tells.
std::map<AtomId, std::size_t> sections_telling(const knowledge::Plan& plan) {
    std::map<AtomId, std::size_t> telling;
    for (std::size_t index = 0; index < plan.sections.size(); ++index) {
        for (const knowledge::Paragraph& paragraph : plan.sections[index].paragraphs) {
            for (const AtomId atom : paragraph.atoms) {
                telling.emplace(atom, index);
            }
        }
    }
    return telling;
}

// The runs of `parsed`: its headings, and the blocks that cite nothing, are
// no section's. Errc::internal for a block that cites an atom, first or not,
// that no section of the plan tells: an article that is not its plan's.
Result<std::vector<Run>> runs_of(const knowledge::ParsedArticle& parsed,
                                 const std::map<AtomId, std::size_t>& telling) {
    std::vector<Run> runs;
    for (const knowledge::Block& block : parsed.blocks) {
        if (block.kind == knowledge::BlockKind::heading) {
            continue;
        }
        const auto citing = std::ranges::find_if(
            block.sentences,
            [](const knowledge::CitingSentence& sentence) { return !sentence.markers.empty(); });
        if (citing == block.sentences.end()) {
            continue;
        }
        for (const knowledge::CitingSentence& sentence : block.sentences) {
            for (const knowledge::Marker& marker : sentence.markers) {
                if (!telling.contains(marker.atom)) {
                    return fail(Errc::internal, std::format("the article is not its plan's: "
                                                            "it cites {}, which no section of its plan tells",
                                                            marker.atom));
                }
            }
        }
        const auto told = telling.find(citing->markers.front().atom);
        if (runs.empty() || runs.back().section != told->second) {
            runs.push_back(Run{.section = told->second});
        }
        runs.back().blocks.push_back(&block);
    }
    return runs;
}

// The wave each seam goes in, the k-th seam being between runs k and k + 1:
// the first, unless it shares its paragraph with a seam of the first, which
// it does when the run between them is one paragraph long.
std::vector<int> waves_of(std::span<const Run> runs) {
    std::vector<int> waves;
    for (std::size_t seam = 0; seam + 1 < runs.size(); ++seam) {
        const bool shares = seam > 0 && runs[seam].blocks.size() == 1 && waves.back() == 1;
        waves.push_back(shares ? 2 : 1);
    }
    return waves;
}

// A seam's two paragraphs, in order: the last of one section, the first of
// the next.
using Sides = std::array<std::string, 2>;

// A seam as the article stands: the last paragraph of one section and the
// first of the next, with the atoms each cites in the order first cited (an
// atom both cite is the first one's).
struct Seam {
    const knowledge::SectionPlan* before = nullptr;
    const knowledge::SectionPlan* after = nullptr;
    std::size_t number = 0;  // of `before` in the plan, from 1
    std::size_t next = 0;    // of `after`
    std::array<text::Span, 2> spans{};
    Sides written{};
    Asked asked{};

    // "the seam between section 1, "Weighing radium", and section 2, "The
    // samples"": the seam, for a finding or a prompt.
    [[nodiscard]] std::string where() const {
        return std::format(R"(the seam between section {}, "{}", and section {}, "{}")", number,
                           before->heading, next, after->heading);
    }
};

Seam seam_at(std::string_view markdown, const knowledge::Plan& plan, std::span<const Run> runs,
             std::size_t index) {
    const Run& ending = runs[index];
    const Run& starting = runs[index + 1];
    Seam seam{.before = &plan.sections[ending.section],
              .after = &plan.sections[starting.section],
              .number = ending.section + 1,
              .next = starting.section + 1};
    const std::array<const knowledge::Block*, 2> blocks{ending.blocks.back(), starting.blocks.front()};
    for (std::size_t side = 0; side < blocks.size(); ++side) {
        seam.spans[side] = blocks[side]->span;
        seam.written[side] = std::string{text::text_of(markdown, blocks[side]->span)};
        seam.asked.paragraphs.emplace_back();
        for (const knowledge::CitingSentence& sentence : blocks[side]->sentences) {
            for (const knowledge::Marker& marker : sentence.markers) {
                if (std::ranges::find(seam.asked.atoms, marker.atom) == seam.asked.atoms.end()) {
                    seam.asked.atoms.push_back(marker.atom);
                    seam.asked.paragraphs.back().push_back(marker.atom);
                }
            }
        }
    }
    return seam;
}

// What the seam's and the judge's prompts show of the video, in their system
// parts: its title and channel.
json::Json shared_of(const Source& source) {
    const json::Json video = video_shared(source.video);
    return json::Json{{"title", video.at("title")}, {"channel", video.at("channel")}};
}

// Two paragraphs as one draft.
std::string joined(const Sides& paragraphs) {
    return paragraphs[0] + "\n\n" + paragraphs[1];
}

// --- The models' part --------------------------------------------------------------------

// A rewrite of a seam, and where it goes.
struct Rewrite {
    std::array<text::Span, 2> spans{};
    Sides paragraphs{};
};

// `markdown` with `kept` in place, from the last back: a rewrite leaves the
// spans before it as they were.
std::string put_in_place(std::string markdown, std::vector<Rewrite> kept) {
    std::ranges::sort(kept, std::greater{}, [](const Rewrite& rewrite) { return rewrite.spans[0].begin; });
    for (const Rewrite& rewrite : kept) {
        markdown.replace(rewrite.spans[1].begin, rewrite.spans[1].size(), rewrite.paragraphs[1]);
        markdown.replace(rewrite.spans[0].begin, rewrite.spans[0].size(), rewrite.paragraphs[0]);
    }
    return markdown;
}

// What the commander and the judge make of the seams: a rewrite of each,
// kept where it is no worse than the seam as written. What they could not
// do is noted in output(), as findings.
class Smoother {
public:
    // The prompts a smoother asks with: its own, and the judge's.
    struct Prompts {
        StagePrompt seam{};
        StagePrompt judge{};
    };

    Smoother(const StageContext& context, const Input& read, agents::Army& army, Prompts prompts)
        : context_(&context)
        , read_(&read)
        , army_(&army)
        , prompts_(std::move(prompts))
        , shared_(shared_of(read.source))
        , telling_(sections_telling(read.plan)) {
        // The judge is the infantry, when it is of another family than the
        // commander that rewrites.
        const agents::Engine* commander = engine_serving(army, "commander");
        const agents::Engine* judge = engine_serving(army, "infantry");
        judge_family_ = commander != nullptr && judge != nullptr && judge->family != commander->family;
    }

    // `markdown` with its seams smoothed, wave by wave (see the header).
    Result<std::string> smoothed(std::string markdown) {
        const knowledge::ParsedArticle parsed = knowledge::parse_article(markdown);
        RR_TRY_ASSIGN(const std::vector<Run> runs, runs_of(parsed, telling_));
        const std::vector<int> waves = waves_of(runs);
        for (const int wave : {1, 2}) {
            std::vector<std::size_t> seams;
            for (std::size_t seam = 0; seam < waves.size(); ++seam) {
                if (waves[seam] == wave) {
                    seams.push_back(seam);
                }
            }
            if (!seams.empty()) {
                RR_TRY_ASSIGN(markdown, smoothed_in_wave(std::move(markdown), seams, wave));
            }
        }
        return markdown;
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

    // One wave: `indices`' seams rewritten, read again where the tier
    // rechecks, and those kept put in place. A rewrite keeps each
    // paragraph's atoms, so the runs of the article stay those of the first
    // wave.
    Result<std::string> smoothed_in_wave(std::string markdown, std::span<const std::size_t> indices,
                                         int wave) {
        const knowledge::ParsedArticle parsed = knowledge::parse_article(markdown);
        RR_TRY_ASSIGN(const std::vector<Run> runs, runs_of(parsed, telling_));
        std::vector<Seam> seams;
        seams.reserve(indices.size());
        for (const std::size_t index : indices) {
            seams.push_back(seam_at(markdown, read_->plan, runs, index));
        }
        RR_TRY_ASSIGN(const std::vector<std::optional<Sides>> rewrites, rewritten(seams, wave));
        RR_TRY_ASSIGN(const Verdicts verdicts, read_again(seams, rewrites));
        std::vector<Rewrite> kept;
        for (std::size_t index = 0; index < seams.size(); ++index) {
            const std::optional<Sides>& rewrite = rewrites[index];
            if (rewrite.has_value() &&
                keeps(seams[index], *rewrite, verdicts.first[index], verdicts.outcomes)) {
                kept.push_back(Rewrite{.spans = seams[index].spans, .paragraphs = *rewrite});
            }
        }
        return put_in_place(std::move(markdown), std::move(kept));
    }

    // `seams` rewritten by the commander, in wave `wave`: each one's rewrite,
    // where one passed its checks and differs from the seam as written;
    // none, and it is said, where none passed.
    Result<std::vector<std::optional<Sides>>> rewritten(std::span<const Seam> seams, int wave) {
        std::vector<std::optional<std::size_t>> asked(seams.size());  // each seam's mission, where held
        std::vector<agents::Mission> missions;
        for (std::size_t index = 0; index < seams.size(); ++index) {
            RR_TRY_ASSIGN(agents::Mission mission, seam_mission(seams[index]));
            if (!army_->holds(mission)) {
                output_.findings.push_back(
                    std::format("{} stays as written: even its two paragraphs are more than {} holds at once",
                                seams[index].where(), model()));
                continue;
            }
            asked[index] = missions.size();
            missions.push_back(std::move(mission));
        }
        if (!missions.empty()) {
            progress(std::format("seams: the commander rewrites {} {}{}", missions.size(),
                                 wave == 1 ? "" : "more ", missions.size() == 1 ? "seam" : "seams"));
        }
        RR_TRY_ASSIGN(const std::vector<agents::Outcome> outcomes,
                      army_->squad(std::move(missions), context_->stop));
        std::vector<std::optional<Sides>> rewrites(seams.size());
        for (std::size_t index = 0; index < seams.size(); ++index) {
            if (const std::optional<std::size_t> mission = asked[index]; mission.has_value()) {
                rewrites[index] = rewrite_of(seams[index], outcomes[*mission]);
            }
        }
        return rewrites;
    }

    // What the judge read: for each seam rewritten, where the tier rechecks,
    // the index in `outcomes` of its reading as written, its reading as
    // rewritten right after.
    struct Verdicts {
        std::vector<std::optional<std::size_t>> first{};
        std::vector<agents::Outcome> outcomes{};
    };

    // `rewrites` read again by the judge, each with its seam as written.
    Result<Verdicts> read_again(std::span<const Seam> seams, std::span<const std::optional<Sides>> rewrites) {
        Verdicts verdicts{.first = std::vector<std::optional<std::size_t>>(seams.size())};
        std::vector<agents::Mission> judges;
        if (read_->tier.seams.mode == reason::SeamsMode::recheck) {
            for (std::size_t index = 0; index < seams.size(); ++index) {
                if (const std::optional<Sides>& rewrite = rewrites[index]; rewrite.has_value()) {
                    RR_TRY(judge_missions(seams[index], *rewrite, verdicts.first[index], judges));
                }
            }
        }
        if (!judges.empty()) {
            const std::size_t count = judges.size() / 2;
            progress(std::format("seams: the infantry reads {} {}, as written and as rewritten", count,
                                 count == 1 ? "seam" : "seams"));
        }
        RR_TRY_ASSIGN(verdicts.outcomes, army_->squad(std::move(judges), context_->stop));
        return verdicts;
    }

    // `id` as a writer is shown it (atom_entry()). Every atom a paragraph
    // cites is the plan's (runs_of()), and so the ledger's (read_input()).
    [[nodiscard]] std::string entry(AtomId id) const {
        const knowledge::Atom* const atom = read_->ledger.find(id);
        RR_ASSERT(atom != nullptr, "a paragraph cites the plan's atoms (runs_of()), the ledger's");
        return atom_entry(*atom, read_->transcript);
    }

    // A mission rewriting `seam`'s two paragraphs.
    [[nodiscard]] Result<agents::Mission> seam_mission(const Seam& seam) const {
        Sides atoms;
        for (std::size_t side = 0; side < atoms.size(); ++side) {
            for (const AtomId id : seam.asked.paragraphs[side]) {
                atoms[side] += (atoms[side].empty() ? "" : "\n") + entry(id);
            }
        }
        const json::Json order{{"number", seam.number},    {"before", seam.before->heading},
                               {"next", seam.next},        {"after", seam.after->heading},
                               {"first_atoms", atoms[0]},  {"first", seam.written[0]},
                               {"second_atoms", atoms[1]}, {"second", seam.written[1]}};
        RR_TRY_ASSIGN(const prompt::Rendered rendered, context_->prompts->render(kPrompt, shared_, order));
        std::string id = std::format("seam-{}", seam.number);
        const std::int64_t seed = llm::derived_seed(context_->job->salt, SeamsStage::kName, id, 0);
        // A rewrite says what the paragraphs say: its room is theirs.
        const auto written = static_cast<std::int64_t>(seam.written[0].size() + seam.written[1].size());
        return agents::Mission{.id = std::move(id),
                               .kind = std::string{kPrompt},
                               .prompt = prompts_.seam.info->label(),
                               .role = "commander",
                               .request = llm::ChatRequest{.messages = {llm::Message::system(rendered.system),
                                                                        llm::Message::user(rendered.user)},
                                                           .seed = seed,
                                                           .thinking = false},
                               .answer_tokens = prose_tokens(written, seam.asked.paragraphs.size()),
                               .check =
                                   [transcript = &read_->transcript, ledger = &read_->ledger,
                                    asked = seam.asked](const json::Json& answer) {
                                       return draft_problems(answer.get<std::string>(), *transcript, *ledger,
                                                             asked);
                                   },
                               .repairs = kRepairs,
                               .escalation = kSeamEscalation};
    }

    // The rewrite of `seam` in `outcome`, when it differs from the seam as
    // written; none, said, when there is none.
    std::optional<Sides> rewrite_of(const Seam& seam, const agents::Outcome& outcome) {
        noted(outcome);
        if (outcome.answers.empty()) {
            output_.findings.push_back(std::format("{} stays as written: {}", seam.where(),
                                                   why_unanswered(outcome, *army_, "rewrite it")));
            return std::nullopt;
        }
        const std::vector<std::string> paragraphs =
            paragraphs_of(outcome.answers.front().value.get<std::string>());
        RR_ASSERT(paragraphs.size() == 2,
                  "a rewrite that passed its checks has two paragraphs (draft_problems())");
        Sides rewrite{paragraphs[0], paragraphs[1]};
        if (rewrite == seam.written) {
            return std::nullopt;  // nothing to choose between
        }
        return rewrite;
    }

    // The judge's missions reading `seam` as written, then as rewritten
    // (`judge-seam-2-written`, `judge-seam-2-rewritten`), added to `judges`
    // where it holds both, `first` their index; else said.
    [[nodiscard]] Result<void> judge_missions(const Seam& seam, const Sides& rewrite,
                                              std::optional<std::size_t>& first,
                                              std::vector<agents::Mission>& judges) {
        if (!judge_family_) {
            if (!unjudged_said_) {
                output_.findings.emplace_back(
                    "the seams' rewrites are kept by Ledger B alone: "
                    "the pod has no judge of another family than the writer's");
                unjudged_said_ = true;
            }
            return {};
        }
        std::string atoms;
        for (const AtomId id : seam.asked.atoms) {
            atoms += (atoms.empty() ? "" : "\n") + entry(id);
        }
        constexpr std::array<std::string_view, 2> kSides{"written", "rewritten"};
        const Sides drafts{joined(seam.written), joined(rewrite)};
        std::array<agents::Mission, 2> missions;
        for (std::size_t side = 0; side < missions.size(); ++side) {
            RR_TRY_ASSIGN(
                missions[side],
                judge_mission(*context_, SeamsStage::kName, prompts_.judge, shared_,
                              ToJudge{.id = std::format("judge-seam-{}-{}", seam.number, kSides[side]),
                                      .what = seam.where(),
                                      .atoms = atoms,
                                      .draft = drafts[side]}));
        }
        if (!army_->holds(missions[0]) || !army_->holds(missions[1])) {
            output_.findings.push_back(std::format(
                "{}: Ledger B alone decides on its rewrite: it is more than the judge holds at once",
                seam.where()));
            return {};
        }
        first = judges.size();
        judges.push_back(std::move(missions[0]));
        judges.push_back(std::move(missions[1]));
        return {};
    }

    // Whether `rewrite` is kept: where the judge read both, no more issues in
    // it than in the seam as written, then no more of Ledger B's warnings
    // (the order of the write stage's pick; a tie keeps the rewrite). What
    // the judge reads in a rewrite kept is said, as its reading; a rewrite
    // dropped, and why.
    bool keeps(const Seam& seam, const Sides& rewrite, std::optional<std::size_t> first,
               std::span<const agents::Outcome> verdicts) {
        std::optional<std::array<std::vector<Judged>, 2>> readings;
        if (first.has_value()) {
            readings = readings_of(seam, verdicts.subspan(*first, 2));
        }
        const std::array<std::size_t, 2> issues{readings.has_value() ? issues_in((*readings)[0]) : 0,
                                                readings.has_value() ? issues_in((*readings)[1]) : 0};
        const std::array<std::size_t, 2> warnings{
            warnings_of(joined(seam.written), read_->transcript, read_->ledger, seam.asked),
            warnings_of(joined(rewrite), read_->transcript, read_->ledger, seam.asked)};
        if (std::tie(issues[1], warnings[1]) > std::tie(issues[0], warnings[0])) {
            output_.findings.push_back(
                issues[1] > issues[0]
                    ? std::format(
                          "{} stays as written: the judge reads more issues in its rewrite ({} against {})",
                          seam.where(), issues[1], issues[0])
                    : std::format(
                          "{} stays as written: Ledger B warns of more in its rewrite ({} against {})",
                          seam.where(), warnings[1], warnings[0]));
            return false;
        }
        if (readings.has_value()) {
            published(seam, joined(rewrite), (*readings)[1]);
        }
        return true;
    }

    // What the judge read in `seam`, as written and as rewritten; none when
    // a reading is missing, which is said.
    std::optional<std::array<std::vector<Judged>, 2>> readings_of(const Seam& seam,
                                                                  std::span<const agents::Outcome> verdicts) {
        for (const agents::Outcome& outcome : verdicts) {
            noted(outcome);
        }
        std::array<std::vector<Judged>, 2> readings;
        for (std::size_t side = 0; side < readings.size(); ++side) {
            const agents::Outcome& outcome = verdicts[side];
            if (outcome.answers.empty()) {
                output_.findings.push_back(
                    std::format("{}: Ledger B alone decides on its rewrite: "
                                "the judge could not read it: {}",
                                seam.where(), why_unanswered(outcome, *army_, "read it")));
                return std::nullopt;
            }
            readings[side] = judgment_of(outcome.answers.front().value);
        }
        return readings;
    }

    // What the judge read in `rewrite`, the one kept, said as its readings.
    void published(const Seam& seam, std::string_view rewrite, std::span<const Judged> reading) {
        const std::vector<std::string> sentences = sentences_of(rewrite);
        for (const Judged& sentence : reading) {
            if (sentence.issue != Issue::none) {
                RR_ASSERT(sentence.sentence >= 1, "a reading numbers sentences from 1 (judgment_problems())");
                RR_ASSERT(sentence.sentence <= sentences.size(),
                          "a reading's sentences are its draft's (judgment_problems())");
                output_.findings.push_back(std::format("{}: the judge reads \"{}\" as {}: {}", seam.where(),
                                                       shown(sentences[sentence.sentence - 1]),
                                                       reading_of(sentence.issue),
                                                       sentence.reading.value_or("it gives no reading")));
            }
        }
    }

    // A ceiling makes the result this job's alone (ADR-0018).
    void noted(const agents::Outcome& outcome) {
        if (outcome.verdict == agents::Verdict::ceiling) {
            output_.reusable = false;
        }
    }

    const StageContext* context_;
    const Input* read_;
    agents::Army* army_;
    Prompts prompts_;
    json::Json shared_;  // the video, as the seam's and the judge's prompts show it
    std::map<AtomId, std::size_t> telling_;
    bool judge_family_ = false;  // a judge of another family than the commander can read
    bool unjudged_said_ = false;
    StageOutput output_;
};

}  // namespace

Result<json::Json> SeamsStage::fingerprint(const StageContext& context) const {
    RR_ASSERT(context.job != nullptr, "a stage runs for a job");
    RR_TRY_ASSIGN(const reason::Tier tier, job_tier(context));
    json::Json key{{"tier", tier.canonical(reason::Stage::seams)}};
    if (tier.seams.mode == reason::SeamsMode::none) {
        return key;
    }
    if (!context.army) {
        return fail(Errc::needs_user,
                    "smoothing the article's seams needs models, and no pod is configured: "
                    "rreport doctor says how to set one up");
    }
    RR_ASSERT(context.prompts != nullptr, "a stage that asks models is given the prompts");
    const bool rechecks = tier.seams.mode == reason::SeamsMode::recheck;
    RR_TRY_ASSIGN(json::Json prompts, rechecks
                                          ? prompts_fingerprint(*context.prompts, {kPrompt, kJudgePrompt})
                                          : prompts_fingerprint(*context.prompts, {kPrompt}));
    RR_TRY_ASSIGN(agents::Army* const army, context.army());
    const agents::Engine* commander = engine_serving(*army, "commander");
    key["prompts"] = std::move(prompts);
    key["army"] = army_fingerprint(*army);
    key["holds"] = commander == nullptr ? 0 : commander->ctx_per_slot;
    if (rechecks) {
        const agents::Engine* judge = engine_serving(*army, "infantry");
        key["judge_holds"] = judge == nullptr ? 0 : judge->ctx_per_slot;
    }
    return key;
}

Result<StageOutput> SeamsStage::run(const StageContext& context) const {
    RR_TRY_ASSIGN(const Input read, read_input(context));
    if (read.tier.seams.mode == reason::SeamsMode::none) {
        return StageOutput{.artifact = knowledge::to_json(knowledge::Article{.markdown = read.article})};
    }
    RR_TRY_ASSIGN(agents::Army* const army, context.army());
    Smoother::Prompts prompts;
    RR_TRY_ASSIGN(prompts.seam, stage_prompt(*context.prompts, kPrompt));
    RR_TRY_ASSIGN(prompts.judge, stage_prompt(*context.prompts, kJudgePrompt));
    Smoother smoother{context, read, *army, std::move(prompts)};
    RR_TRY_ASSIGN(const std::string markdown, smoother.smoothed(read.article));
    StageOutput output = std::move(smoother.output());
    if (markdown != read.article) {
        // What Ledger B finds in the article whole that it did not find in it
        // as written: the write stage said the rest.
        const fidelity::ArticleScope scope{.duplicates = read.plan.duplicates};
        std::set<std::string, std::less<>> before;
        for (const fidelity::Finding& finding :
             fidelity::check_article(read.transcript, read.ledger, read.article, scope).findings) {
            before.insert(finding.message);
        }
        for (const fidelity::Finding& finding :
             fidelity::check_article(read.transcript, read.ledger, markdown, scope).findings) {
            if (!before.contains(finding.message)) {
                output.findings.push_back(finding.message);
            }
        }
    }
    output.artifact = knowledge::to_json(knowledge::Article{.markdown = markdown});
    return output;
}

}  // namespace rr::pipeline

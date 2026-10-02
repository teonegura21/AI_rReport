// The plan stage on the synthetic lecture (lecture.hpp) and the ledger the
// oracle makes of it: every section planned by the commander, thinking, in a
// closed world; every rule an answer breaks sent back; what no answer
// planned told in the order said, and said; with the faults real models make
// injected where a test says.
#include <rr/pipeline/plan_stage.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/json/json.hpp>
#include <rr/knowledge/atoms.hpp>
#include <rr/knowledge/outline.hpp>
#include <rr/knowledge/plan.hpp>
#include <rr/knowledge/transcript.hpp>
#include <rr/oracle/oracle.hpp>
#include <rr/pipeline/source.hpp>
#include <rr/prompt/tokens.hpp>
#include <rr/reason/tier.hpp>
#include <rr/testkit/result.hpp>

#include "lecture.hpp"
#include "oracle_army.hpp"
#include "variant_tiers.hpp"

namespace rr::pipeline {
namespace {

using knowledge::AtomId;
using knowledge::AtomLedger;
using knowledge::Paragraph;
using knowledge::Plan;
using knowledge::PlannedBy;
using knowledge::SectionPlan;
using knowledge::SentenceId;
using testing::atom_ids;
using ::testing::Contains;
using ::testing::ElementsAre;
using testing::engines;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using testing::lecture;
using testing::lecture_ledger;
using testing::lecture_outline;
using testing::lecture_plan;
using testing::oracle_paragraph;
using testing::OracleArmy;
using testing::reworded;
using testing::source;
using testing::without;
using testkit::unwrap;

// --- What a plan should be ------------------------------------------------------------

// A paragraph grouped in the order said, without a purpose.
Paragraph grouped_paragraph(std::uint32_t first, std::uint32_t last) {
    return Paragraph{.atoms = atom_ids(first, last)};
}

// `section` of the lecture's plan told in the order said, as no model planned it.
SectionPlan grouped_section(std::size_t section) {
    const std::vector<std::vector<Paragraph>> paragraphs{
        {grouped_paragraph(1, 6), grouped_paragraph(7, 7)},
        {grouped_paragraph(8, 13), grouped_paragraph(14, 17)},
        {grouped_paragraph(18, 23), grouped_paragraph(24, 26)}};
    const Plan plan = lecture_plan();
    const SectionPlan& planned = plan.sections.at(section);
    return SectionPlan{.first = planned.first,
                       .heading = planned.heading,
                       .paragraphs = paragraphs.at(section),
                       .planned_by = PlannedBy::grouping};
}

// --- What was asked ---------------------------------------------------------------------

// The requests that opened each plan mission, in the order of their ids.
std::vector<llm::ChatRequest> opened(const OracleArmy& rig) {
    std::vector<llm::ChatRequest> first;
    for (const llm::ChatRequest& request : rig.requests("plan")) {
        if (first.empty() || first.back().tag.mission_id != request.tag.mission_id) {
            first.push_back(request);
        }
    }
    return first;
}

std::vector<std::string> missions_of(const OracleArmy& rig) {
    std::vector<std::string> ids;
    for (const llm::ChatRequest& request : opened(rig)) {
        ids.push_back(request.tag.mission_id);
    }
    return ids;
}

// The last message of every request of `mission`, in the order sent: its
// order, then what each repair sent back.
std::vector<std::string> sent_to(const OracleArmy& rig, std::string_view mission) {
    std::vector<std::string> messages;
    for (const llm::ChatRequest& request : rig.requests("plan")) {
        if (request.tag.mission_id == mission) {
            messages.push_back(request.messages.back().text());
        }
    }
    return messages;
}

class PlanStageTest : public ::testing::Test {
protected:
    Result<StageOutput> run(RaiseArmy army, const AtomLedger& ledger = lecture_ledger(),
                            const knowledge::Outline& outline = lecture_outline(),
                            const knowledge::Transcript& transcript = lecture()) {
        StageContext context{.job = &job_,
                             .army = std::move(army),
                             .prompts = &prompts_,
                             .progress = [this](std::string_view line) { progress_.emplace_back(line); },
                             .tiers = tiers_};
        context.inputs.emplace("source", to_json(source()));
        context.inputs.emplace("transcript", knowledge::to_json(transcript));
        context.inputs.emplace("outline", knowledge::to_json(outline));
        context.inputs.emplace("atomize", knowledge::to_json(ledger));
        RR_TRY_ASSIGN(context.fingerprint, stage_.fingerprint(context));
        fingerprint_ = context.fingerprint;
        return stage_.run(context);
    }

    // The plan `output` holds, which must hold the ledger as the outline cuts it.
    static Plan plan_of(const StageOutput& output, const AtomLedger& ledger = lecture_ledger()) {
        Plan plan = unwrap(knowledge::plan_from_json(output.artifact));
        EXPECT_THAT(knowledge::plan_problems(plan, ledger, lecture_outline()), IsEmpty());
        return plan;
    }

    void use_tier(std::string tier) { job_.tier = std::move(tier); }

    // A tier of the test's own, named `name`: `max`, changed as `change` says.
    void use_variant(std::string name, std::function<void(reason::Tier&)> change) {
        tiers_ = testing::variant_of("max", std::move(change));
        job_.tier = std::move(name);
    }

    [[nodiscard]] const json::Json& fingerprint() const { return fingerprint_; }
    [[nodiscard]] const std::vector<std::string>& progress() const { return progress_; }

private:
    library::Job job_{.id = "J1", .tier = "max"};
    const prompt::Library prompts_ = unwrap(prompt::Library::embedded());
    const PlanStage stage_;
    FindTier tiers_;
    json::Json fingerprint_;
    std::vector<std::string> progress_;
};

// --- Every section planned ------------------------------------------------------------------

TEST_F(PlanStageTest, EverySectionIsPlannedByTheCommanderThinking) {
    OracleArmy rig;
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, IsEmpty());
    EXPECT_TRUE(output.reusable);
    EXPECT_EQ(plan_of(output), lecture_plan());
    EXPECT_THAT(missions_of(rig), ElementsAre("plan-1", "plan-2", "plan-3")) << "a mission a section";
    for (const llm::ChatRequest& request : opened(rig)) {
        EXPECT_TRUE(request.thinking.value_or(false)) << request.tag.mission_id;
        ASSERT_TRUE(request.thinking_budget.has_value());
        EXPECT_EQ(request.thinking_budget->tokens, 16'384) << "the max tier's, within half of what it holds";
        EXPECT_EQ(request.thinking_budget->closing, "Answer now.") << "the commander's own words";
        EXPECT_FALSE(request.n.has_value()) << "one plan a section";
    }
    EXPECT_THAT(progress(), ElementsAre("plan: the commander plans 3 sections"));
}

TEST_F(PlanStageTest, TheCommandersHeadingNamesItsSection) {
    OracleArmy rig{{reworded("plan-1", [](json::Json& answer) { answer["heading"] = "Radium, weighed"; })}};
    Plan expected = lecture_plan();
    expected.sections.at(0).heading = "Radium, weighed";
    EXPECT_EQ(plan_of(unwrap(run(rig.raise()))), expected) << "not the outline's title";
}

TEST_F(PlanStageTest, TheCommanderIsShownTheArticlesSectionsAndTheSectionsAtomsInOrder) {
    AtomLedger ledger = lecture_ledger();
    knowledge::Atom& pierre = ledger.atoms.at(25);  // A26, said at S29
    pierre.speaker = "Ada Lovelace";
    pierre.attributed_to = "Pierre";
    pierre.register_kind = knowledge::Register::aside;
    OracleArmy rig;
    unwrap(run(rig.raise(), ledger));
    const std::vector<llm::ChatRequest> requests = opened(rig);
    ASSERT_EQ(requests.size(), 3U);
    const std::string system = requests.at(0).messages.at(0).text();
    EXPECT_THAT(system, HasSubstr("The video is \"Weighing radium\", by Science Hour."));
    EXPECT_THAT(system, HasSubstr("What the whole video says:\nAda weighs radium, sample by sample."));
    EXPECT_THAT(system,
                HasSubstr("The article's sections, in order:\n1. [0:00] Weighing radium\n2. [5:00] The "
                          "samples\n3. [10:00] What they mean"));
    for (const llm::ChatRequest& request : requests) {
        EXPECT_EQ(request.messages.at(0).text(), system) << "one system part, for the engine's cache";
    }
    const std::string first = requests.at(0).messages.at(1).text();
    EXPECT_THAT(first,
                HasSubstr("Plan section 1, \"Weighing radium\", from 0:00 to 4:55.\n\nIts atoms, in the "
                          "order said:\n"))
        << "no gist and no part: nothing between";
    EXPECT_THAT(first,
                HasSubstr("<atoms>\nA1 [0:30] claim: I'm Ada, and today we weigh radium.\nA2 [1:30] "
                          "number: The NRC lent us 3 scales for it.\nA3 [2:00] claim, hedged: Maybe the "
                          "scales are older than the lab.\n"));
    EXPECT_THAT(first,
                HasSubstr("A6 [4:00] number: In 1902 Marie Curie called radium \"the most precious "
                          "thing\". (to quote: \"the most precious thing\")\nA7 [4:30] question: Why does "
                          "it glow?\n</atoms>"))
        << "a question's modality is its kind: said once";
    EXPECT_THAT(requests.at(2).messages.at(1).text(),
                HasSubstr("A26 [14:00] claim, hedged, aside, said by Ada Lovelace, reporting Pierre: I think "
                          "the numbers mean we need better scales, Pierre."));
}

TEST_F(PlanStageTest, AFastTierTellsTheAtomsInTheOrderSaidWithoutAModel) {
    use_tier("fast");
    const StageOutput output = unwrap(run({}));
    EXPECT_THAT(output.findings, IsEmpty()) << "the tier's choice, not a failure";
    EXPECT_EQ(plan_of(output),
              (Plan{.sections = {grouped_section(0), grouped_section(1), grouped_section(2)}}));
    EXPECT_THAT(progress(), ElementsAre("plan: the atoms are told in the order said, a paragraph at a time"));
    EXPECT_EQ(fingerprint().at("models"), nullptr) << "no pod needed";
}

TEST_F(PlanStageTest, AnAtomSaidAgainIsDeclaredADuplicateWhereItLosesNothing) {
    AtomLedger ledger = lecture_ledger();
    ledger.atoms.at(4).text = ledger.atoms.at(3).text;  // A5 says what A4 says
    OracleArmy rig;
    const StageOutput output = unwrap(run(rig.raise(), ledger));
    EXPECT_THAT(output.findings, IsEmpty());
    Plan expected = lecture_plan();
    expected.sections.at(0).paragraphs = {
        oracle_paragraph(1, 3),
        Paragraph{.purpose = "Tells A4 to A7.", .atoms = {AtomId{4}, AtomId{6}, AtomId{7}}}};
    expected.duplicates = {
        knowledge::Duplicate{.atom = AtomId{5}, .of = AtomId{4}, .because = "It says again what A4 says."}};
    EXPECT_EQ(plan_of(output, ledger), expected);
}

// --- Checks, and repairs --------------------------------------------------------------------

TEST_F(PlanStageTest, EveryRuleAnAnswerBreaksIsSentBackAndRepaired) {
    struct Case {
        std::string_view why;
        std::function<void(json::Json&)> change;
        std::string sent_back;
    };
    const auto paragraph = [](json::Json& answer, std::size_t index) -> json::Json& {
        return answer.at("paragraphs").at(index);
    };
    // A7, the question, out of its paragraph and declared a duplicate of `of`.
    const auto declared = [](std::string atom, std::string of) {
        return [atom = std::move(atom), of = std::move(of)](json::Json& answer) {
            answer.at("paragraphs").erase(2);
            answer.at("duplicates").push_back(json::Json{{"because", "Again."}, {"atom", atom}, {"of", of}});
        };
    };
    std::string thirty_one;
    for (int word = 0; word < 31; ++word) {
        thirty_one += "word ";
    }
    const std::vector<Case> cases{
        {"an atom of another section",
         [&](json::Json& answer) { paragraph(answer, 0).at("atoms").push_back("A8"); },
         "A8 is not among the atoms to plan (A1–A7): plan those only"},
        {"an atom told twice", [&](json::Json& answer) { paragraph(answer, 1).at("atoms").push_back("A1"); },
         "A1 is told twice: tell each atom once, in one paragraph"},
        {"an atom left out", [](json::Json& answer) { answer.at("paragraphs").erase(2); },
         "A7 is in no paragraph: tell it, or declare it a duplicate of the atom that says all it says"},
        {"a duplicate of itself", declared("A7", "A7"),
         "A7 is declared a duplicate of itself: name the atom that tells what it says, or tell it"},
        {"a duplicate of an atom no paragraph tells", declared("A7", "A9"),
         "A7 is declared a duplicate of A9, which no paragraph tells: name an atom a paragraph tells, or "
         "tell it"},
        {"a duplicate that would lose how it is said", declared("A7", "A3"),
         "A7 is declared a duplicate of A3, yet it is a question, and A3 hedged: tell it"},
        {"an atom told and declared a duplicate",
         [](json::Json& answer) {
             answer.at("duplicates")
                 .push_back(json::Json{{"because", "Again."}, {"atom", "A1"}, {"of", "A2"}});
         },
         "A1 is told and declared a duplicate: it is one or the other"},
        {"a duplicate declared twice",
         [&](json::Json& answer) {
             declared("A7", "A3")(answer);
             answer.at("duplicates").push_back(answer.at("duplicates").at(0));
         },
         "A7 is declared a duplicate twice: once is enough"},
        {"a duplicate of another section",
         [](json::Json& answer) {
             answer.at("duplicates")
                 .push_back(json::Json{{"because", "Again."}, {"atom", "A9"}, {"of", "A1"}});
         },
         "A9 is not among the atoms to plan (A1–A7): declare those only"},
        {"a paragraph too long",
         [&](json::Json& answer) {
             for (const char* atom : {"A1", "A2", "A3", "A4", "A5", "A6", "A7", "A1", "A2", "A3"}) {
                 paragraph(answer, 0).at("atoms").push_back(atom);
             }
         },
         "paragraph 1 tells 13 atoms: split it, 12 at most"},
        {"a purpose of more than a sentence",
         [&](json::Json& answer) { paragraph(answer, 0)["purpose"] = thirty_one; },
         "paragraph 1's purpose has 31 words: say what a reader learns in one sentence, 30 words at most"},
        {"a heading too long",
         [](json::Json& answer) {
             answer["heading"] = "How Ada weighed radium in the shed with three old scales lent by them";
         },
         "the heading has 14 words: 12 at most"},
    };
    for (const Case& test : cases) {
        SCOPED_TRACE(test.why);
        OracleArmy rig{{reworded("plan-1", test.change)}};
        const StageOutput output = unwrap(run(rig.raise()));
        EXPECT_THAT(output.findings, IsEmpty());
        EXPECT_EQ(plan_of(output), lecture_plan()) << "repaired";
        EXPECT_THAT(sent_to(rig, "plan-1"), Contains(HasSubstr(test.sent_back)));
    }
}

TEST_F(PlanStageTest, ADuplicateThatWouldLoseANumberOrATermIsSentBackForEach) {
    // A2, the NRC's three scales, out of its paragraph and declared A1's.
    OracleArmy rig{{reworded("plan-1", [](json::Json& answer) {
        answer.at("paragraphs").at(0).at("atoms").erase(1);
        answer.at("duplicates").push_back(json::Json{{"because", "Again."}, {"atom", "A2"}, {"of", "A1"}});
    })}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_EQ(plan_of(output), lecture_plan());
    const std::vector<std::string> sent = sent_to(rig, "plan-1");
    ASSERT_EQ(sent.size(), 2U);
    EXPECT_THAT(sent.at(1),
                HasSubstr("A2 is declared a duplicate of A1, yet it says \"3\", which A1 does not: "
                          "tell it"));
    EXPECT_THAT(
        sent.at(1),
        HasSubstr("A2 is declared a duplicate of A1, yet it uses \"NRC\", which A1 does not: tell it"));
}

// --- What no model could do -----------------------------------------------------------------

TEST_F(PlanStageTest, ASectionTheModelsDeclineIsToldInTheOrderSaidAndSaid) {
    OracleArmy rig{{oracle::refusal(oracle::When{.mission = "plan-2", .calls = 0})}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings,
                ElementsAre("section 2, \"The samples\": its atoms are told in the order said, a "
                            "paragraph at a time: the models declined to plan it"));
    EXPECT_TRUE(output.reusable) << "they would decline again";
    Plan expected = lecture_plan();
    expected.sections.at(1) = grouped_section(1);
    EXPECT_EQ(plan_of(output), expected);
}

TEST_F(PlanStageTest, ASectionWhoseSentencesAreAllFillerHasNoParagraph) {
    const AtomLedger fillers = without(lecture_ledger(), {21, 22, 23, 24, 25, 26, 27, 28, 29});
    OracleArmy rig;
    const StageOutput output = unwrap(run(rig.raise(), fillers));
    EXPECT_THAT(output.findings, IsEmpty());
    EXPECT_THAT(missions_of(rig), ElementsAre("plan-1", "plan-2")) << "nothing to ask of the third";
    const Plan plan = plan_of(output, fillers);
    ASSERT_EQ(plan.sections.size(), 3U);
    EXPECT_EQ(plan.sections.at(2), (SectionPlan{.first = SentenceId{21}, .heading = "What they mean"}));
    EXPECT_THAT(progress(), ElementsAre("plan: the commander plans 2 sections"));
}

TEST_F(PlanStageTest, AJobCeilingMakesTheResultThisJobsAlone) {
    OracleArmy rig{{}, engines({.commander_slots = 1}), agents::Ceilings{.completion_tokens = 1}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_FALSE(output.reusable) << "a job allowed more would have planned more";
    EXPECT_THAT(output.findings, Contains(HasSubstr("the job reached its completion_tokens ceiling first")));
    EXPECT_EQ(plan_of(output).sections.at(2), grouped_section(2));
}

TEST_F(PlanStageTest, WithoutAPodThereIsNoPlanAtATierThatPlansWithTheCommander) {
    const Result<StageOutput> output = run({});
    EXPECT_ERRC(output, Errc::needs_user);
    EXPECT_THAT(output.error().message(), HasSubstr("planning at the tier 'max' needs models, and no pod is "
                                                    "configured: rreport doctor says how to set one up"));
}

// --- The commander's size -------------------------------------------------------------------

TEST_F(PlanStageTest, TheCommanderThinksWithinHalfOfWhatItHolds) {
    OracleArmy rig{{}, engines({.commander_holds = 16'384})};
    unwrap(run(rig.raise()));
    ASSERT_THAT(missions_of(rig), ElementsAre("plan-1", "plan-2", "plan-3"))
        << "with half left for its atoms, it holds each section";
    for (const llm::ChatRequest& request : opened(rig)) {
        ASSERT_TRUE(request.thinking_budget.has_value());
        EXPECT_EQ(request.thinking_budget->tokens, 8'192) << request.tag.mission_id;
    }
}

TEST_F(PlanStageTest, ASectionTooLongForTheCommanderIsPlannedInParts) {
    OracleArmy rig{{reworded("plan-2-2", [](json::Json& answer) { answer["heading"] = "Sample 17"; })},
                   engines({.commander_holds = 3'712})};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, IsEmpty()) << "every atom planned by the commander";
    EXPECT_THAT(missions_of(rig), ElementsAre("plan-1", "plan-2-1", "plan-2-2", "plan-3"));
    const std::vector<llm::ChatRequest> requests = opened(rig);
    EXPECT_THAT(requests.at(1).messages.at(1).text(),
                HasSubstr("from 5:00 to 9:55.\nIts atoms are too many to plan at once: plan A8–A16 here; the "
                          "others are planned apart.\n\nIts atoms, in the order said:\n<atoms>\nA8 "));
    EXPECT_THAT(requests.at(2).messages.at(1).text(), HasSubstr("plan A17 here"));
    EXPECT_EQ(plan_of(output), lecture_plan())
        << "the parts' paragraphs joined in order, the first's heading";
    EXPECT_THAT(progress(), ElementsAre("plan: the commander plans 3 sections, in 4 parts"));
}

TEST_F(PlanStageTest, AnAtomTooLongForTheCommanderEvenAloneIsToldInTheOrderSaid) {
    AtomLedger ledger = lecture_ledger();
    std::string endless = "Maybe the scales are older than the lab";
    while (endless.size() < 3'000) {
        endless += ", and older still";
    }
    ledger.atoms.at(2).text = endless + ".";  // A3
    OracleArmy rig{{}, engines({.commander_holds = 4'096})};
    const StageOutput output = unwrap(run(rig.raise(), ledger));
    EXPECT_THAT(output.findings,
                ElementsAre("section 1, \"Weighing radium\": A3 is told in the order said, a "
                            "paragraph at a time: even alone, it is more than qwen-commander "
                            "holds at once"));
    EXPECT_THAT(missions_of(rig), ElementsAre("plan-1-1", "plan-1-2", "plan-2", "plan-3"))
        << "the atoms before it, and those after";
    Plan expected = lecture_plan();
    expected.sections.at(0).paragraphs = {oracle_paragraph(1, 2), grouped_paragraph(3, 3),
                                          oracle_paragraph(4, 6), oracle_paragraph(7, 7)};
    EXPECT_EQ(plan_of(output, ledger), expected);

    // A section whose every atom is that long is none of the commander's.
    for (std::size_t index = 0; index < 7; ++index) {  // A1 to A7
        ledger.atoms.at(index).text = endless + ".";
    }
    OracleArmy longer{{}, engines({.commander_holds = 4'096})};
    const StageOutput none = unwrap(run(longer.raise(), ledger));
    EXPECT_THAT(missions_of(longer), ElementsAre("plan-2", "plan-3"));
    EXPECT_THAT(none.findings,
                ElementsAre("section 1, \"Weighing radium\": its atoms are told in the order said, "
                            "a paragraph at a time: even alone, each is more than qwen-commander "
                            "holds at once"));
    EXPECT_THAT(progress(), ElementsAre("plan: the commander plans 3 sections, in 4 parts",
                                        "plan: the commander plans 2 sections"))
        << "the sections it was asked about";
}

TEST_F(PlanStageTest, ACommanderThatHoldsNoAtomEvenAloneTellsEachSectionInTheOrderSaid) {
    OracleArmy rig{{}, engines({.commander_holds = 2'048})};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(missions_of(rig), IsEmpty());
    EXPECT_THAT(
        output.findings,
        ElementsAre("section 1, \"Weighing radium\": its atoms are told in the order said, a paragraph at a "
                    "time: even alone, each is more than qwen-commander holds at once",
                    "section 2, \"The samples\": its atoms are told in the order said, a paragraph at a "
                    "time: even alone, each is more than qwen-commander holds at once",
                    "section 3, \"What they mean\": its atoms are told in the order said, a paragraph at a "
                    "time: even alone, each is more than qwen-commander holds at once"))
        << "one finding a section, not one an atom";
    EXPECT_EQ(plan_of(output),
              (Plan{.sections = {grouped_section(0), grouped_section(1), grouped_section(2)}}));
    EXPECT_THAT(progress(), IsEmpty()) << "the commander plans nothing";
}

TEST_F(PlanStageTest, AnAnswerMayTakeFortyEightTokensAnAtom) {
    OracleArmy rig;
    unwrap(run(rig.raise()));
    const std::vector<llm::ChatRequest> requests = opened(rig);
    ASSERT_EQ(requests.size(), 3U);
    // The thinking, its closing words, then the answer (rr/agents/army.hpp).
    const int thinking = 16'384 + static_cast<int>(prompt::TokenEstimator{}.estimate("Answer now."));
    EXPECT_EQ(requests.at(0).max_tokens, 512 + thinking) << "seven atoms: the least an answer takes";
    EXPECT_EQ(requests.at(1).max_tokens, (10 * 48) + 128 + thinking) << "ten atoms, and room besides";
}

// --- What is refused, and the key -------------------------------------------------------------

TEST_F(PlanStageTest, AnOutlineThatDoesNotCoverItsTranscriptIsABug) {
    OracleArmy rig;
    knowledge::Outline outline = lecture_outline();
    outline.sections.pop_back();
    EXPECT_ERRC(run(rig.raise(), lecture_ledger(), outline), Errc::internal);
}

TEST_F(PlanStageTest, ALedgerThatSaysWhatTheTranscriptDoesNotIsABug) {
    AtomLedger ledger = lecture_ledger();
    ledger.atoms.back().sentences.emplace_back(31);
    OracleArmy rig;
    const Result<StageOutput> output = run(rig.raise(), ledger);
    EXPECT_ERRC(output, Errc::internal);
    EXPECT_THAT(output.error().message(),
                HasSubstr("the ledger's A26 is said at S31, past the transcript's last sentence, S30"));
}

TEST_F(PlanStageTest, ATierRReportDoesNotShipIsNotFound) {
    use_tier("turbo");
    EXPECT_ERRC(run({}), Errc::not_found);
}

TEST_F(PlanStageTest, TheKeyHoldsTheTierThePromptTheArmyAndWhatTheCommanderHolds) {
    OracleArmy rig;
    unwrap(run(rig.raise()));
    const json::Json max = fingerprint();
    EXPECT_EQ(
        max.at("limits"),
        (json::Json{
            {"paragraph_atoms", 12}, {"heading_words", 12}, {"purpose_words", 30}, {"grouped_atoms", 6}}));
    EXPECT_EQ(max.at("models").at("prompts").at("plan").at("label"), "plan@1");
    EXPECT_EQ(max.at("models").at("prompts").at("plan").at("hash"),
              unwrap(unwrap(prompt::Library::embedded()).content_hash("plan")));
    EXPECT_EQ(max.at("models").at("holds"), 32'768);
    EXPECT_EQ(max.at("models").at("army").size(), 2U);

    OracleArmy smaller{{}, engines({.commander_holds = 16'384})};
    unwrap(run(smaller.raise()));
    EXPECT_EQ(fingerprint().at("models").at("holds"), 16'384);

    use_variant("max-lean", [](reason::Tier& tier) { tier.plan.thinking_budget = 4'096; });
    OracleArmy lean;
    unwrap(run(lean.raise()));
    EXPECT_NE(fingerprint().at("tier"), max.at("tier"));
    EXPECT_EQ(fingerprint().at("models"), max.at("models"));
}

}  // namespace
}  // namespace rr::pipeline

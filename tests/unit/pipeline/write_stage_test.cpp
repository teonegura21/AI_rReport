// The write stage on the synthetic lecture (lecture.hpp), its ledger and the
// plan the oracle makes of them: every section written by the commander,
// every atom told and cited; every rule a draft breaks sent back; what no
// draft passed told as its atoms say it, and said; with the faults real
// models make injected where a test says.
#include <rr/pipeline/write_stage.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/core/strings.hpp>
#include <rr/json/json.hpp>
#include <rr/knowledge/article.hpp>
#include <rr/knowledge/atoms.hpp>
#include <rr/knowledge/outline.hpp>
#include <rr/knowledge/plan.hpp>
#include <rr/knowledge/transcript.hpp>
#include <rr/llm/memo.hpp>
#include <rr/oracle/oracle.hpp>
#include <rr/pipeline/source.hpp>
#include <rr/reason/tier.hpp>
#include <rr/testkit/result.hpp>

#include "lecture.hpp"
#include "oracle_army.hpp"
#include "variant_tiers.hpp"

namespace rr::pipeline {
namespace {

using knowledge::Atom;
using knowledge::AtomId;
using knowledge::AtomLedger;
using knowledge::Plan;
using knowledge::SectionPlan;
using ::testing::Contains;
using ::testing::Each;
using ::testing::ElementsAre;
using testing::engines;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using testing::lecture;
using testing::lecture_ledger;
using testing::lecture_outline;
using testing::lecture_plan;
using ::testing::Not;
using testing::oracle_article;
using testing::oracle_paragraph;
using testing::oracle_paragraphs;
using testing::OracleArmy;
using testing::reworded;
using testing::source;
using ::testing::UnorderedElementsAre;
using testing::without;
using testkit::unwrap;

// --- What the article should be -------------------------------------------------------------

// A sentence of 129 bytes whose "\u00e9" spans its 120th and 121st: one a
// message shows cut, before that character.
constexpr std::string_view kLongSentence =
    "The lab was cold that winter, and the old scales creaked each time a sample was set on their pan "
    "by Ada, who sipped caf\u00e9 slowly.";

// The lecture's sample sentence `number`, as said (lecture.hpp).
std::string sample(int number) {
    return testing::lecture_texts().at(static_cast<std::size_t>(number) - 1);
}

// The lecture's first paragraph, as the oracle writes it.
constexpr std::string_view kFirstParagraph =
    "I'm Ada, and today we weigh radium. [A1] The NRC lent us 3 scales for it. [A2] Maybe the scales are "
    "older than the lab. [A3]";

// What Ledger B says of the first paragraph reordered: of A2, the one atom
// to move to keep the talk's order.
constexpr std::string_view kReorderedWarning =
    "A2 was said at S4, before what this section tells ahead of it: keep the order of the talk";

// --- Faults, and what was asked ------------------------------------------------------------

// The lecture's first paragraph with A1 told after A2: out of the talk's
// order, a warning only.
std::string reordered(const std::string& draft) {
    return replace_all(draft,
                       "I'm Ada, and today we weigh radium. [A1] The NRC lent us 3 scales for it. [A2]",
                       "The NRC lent us 3 scales for it. [A2] I'm Ada, and today we weigh radium. [A1]");
}

// A fault that rewrites the draft of the first call of `mission` as `change` says.
oracle::Fault redrafted(std::string mission, std::function<std::string(const std::string&)> change) {
    return oracle::edit("redrafted", oracle::When{.mission = std::move(mission)},
                        [change = std::move(change)](const std::string& draft) { return change(draft); });
}

// A fault that rewrites each draft of `mission` as `change` says, given its
// number (1: the first call): drafts drawn one call at a time, on a commander
// of one slot.
oracle::Fault on_drafts(std::string mission, std::function<std::string(int, const std::string&)> change) {
    auto calls = std::make_shared<int>(0);
    return oracle::edit(
        "on drafts", oracle::When{.mission = std::move(mission), .calls = 0},
        [calls, change = std::move(change)](const std::string& draft) { return change(++*calls, draft); });
}

// The requests that opened each write mission, in the order of their ids.
std::vector<llm::ChatRequest> opened(const OracleArmy& rig) {
    std::vector<llm::ChatRequest> first;
    for (const llm::ChatRequest& request : rig.requests("write")) {
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

// The last message of every request of `mission`, in the order sent.
std::vector<std::string> sent_to(const OracleArmy& rig, std::string_view mission) {
    std::vector<std::string> messages;
    for (const llm::ChatRequest& request : rig.requests("write")) {
        if (request.tag.mission_id == mission) {
            messages.push_back(request.messages.back().text());
        }
    }
    return messages;
}

// The missions of `kind` asked, each once, by id (the rig sorts them).
std::vector<std::string> asked(const OracleArmy& rig, std::string_view kind) {
    std::vector<std::string> ids;
    for (const llm::ChatRequest& request : rig.requests(kind)) {
        if (std::ranges::find(ids, request.tag.mission_id) == ids.end()) {
            ids.push_back(request.tag.mission_id);
        }
    }
    return ids;
}

// A judge's answer changed to read its `sentence`th sentence (from 1) as
// `issue`, for `why`.
std::function<void(json::Json&)> read_as(std::size_t sentence, std::string issue, std::string why) {
    return [sentence, issue = std::move(issue), why = std::move(why)](json::Json& judged) {
        json::Json& item = judged.at("sentences").at(sentence - 1);
        item["issue"] = issue;
        item["reading"] = why;
    };
}

// The lecture's first paragraph with A1 said in other words: as faithful,
// and longer by a word (the oracle's flow judge prefers the shorter).
std::string in_other_words(const std::string& draft) {
    return replace_all(draft, "I'm Ada, and", "I am Ada, and");
}

class WriteStageTest : public ::testing::Test {
protected:
    Result<StageOutput> run(RaiseArmy army, const Plan& plan = lecture_plan(),
                            const AtomLedger& ledger = lecture_ledger(),
                            const knowledge::Outline& outline = lecture_outline()) {
        StageContext context{.job = &job_,
                             .army = std::move(army),
                             .prompts = &prompts_,
                             .progress = [this](std::string_view line) { progress_.emplace_back(line); },
                             .tiers = tiers_};
        context.inputs.emplace("source", to_json(source()));
        context.inputs.emplace("transcript", knowledge::to_json(lecture()));
        context.inputs.emplace("outline", knowledge::to_json(outline));
        context.inputs.emplace("atomize", knowledge::to_json(ledger));
        context.inputs.emplace("plan", knowledge::to_json(plan));
        RR_TRY_ASSIGN(context.fingerprint, stage_.fingerprint(context));
        fingerprint_ = context.fingerprint;
        return stage_.run(context);
    }

    static std::string article_of(const StageOutput& output) {
        return unwrap(knowledge::article_from_json(output.artifact)).markdown;
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
    const WriteStage stage_;
    FindTier tiers_;
    json::Json fingerprint_;
    std::vector<std::string> progress_;
};

// --- Every section written -------------------------------------------------------------------

TEST_F(WriteStageTest, EverySectionIsWrittenByTheCommanderAndEveryAtomTold) {
    OracleArmy rig;
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, IsEmpty()) << "Ledger B holds";
    EXPECT_TRUE(output.reusable);
    const std::string article = article_of(output);
    EXPECT_EQ(article, oracle_article());
    EXPECT_TRUE(article.starts_with(std::format("## Weighing radium\n\n{}\n\n", kFirstParagraph)));
    EXPECT_THAT(missions_of(rig), ElementsAre("write-1", "write-2", "write-3")) << "a mission a section";
    for (const llm::ChatRequest& request : opened(rig)) {
        EXPECT_EQ(request.n, 4) << "the max tier's drafts, drawn at once";
        EXPECT_FALSE(request.thinking.value_or(false)) << "writing does not think at max";
        EXPECT_EQ(request.seed, llm::derived_seed(0, "write", request.tag.mission_id, 0)) << "each its own";
    }
    EXPECT_THAT(progress(), ElementsAre("write: the commander writes 3 sections, 4 drafts each",
                                        "write: the infantry judges the best 2 drafts of 3 sections"));
}

TEST_F(WriteStageTest, TheCommanderIsShownTheSectionsParagraphsAndTheirAtomsInFull) {
    AtomLedger ledger = lecture_ledger();
    ledger.atoms.at(1).terms = {"NRC", "scales"};  // A2
    Atom& maybe = ledger.atoms.at(2);              // A3
    maybe.register_kind = knowledge::Register::aside;
    maybe.intensity = knowledge::Intensity::high;
    Atom& pierre = ledger.atoms.at(25);  // A26
    pierre.speaker = "Ada Lovelace";
    pierre.attributed_to = "Pierre";
    Plan plan = lecture_plan();
    plan.sections.at(2).paragraphs.at(0).purpose.reset();  // grouped: no purpose
    OracleArmy rig;
    unwrap(run(rig.raise(), plan, ledger));
    const std::vector<llm::ChatRequest> requests = opened(rig);
    ASSERT_EQ(requests.size(), 3U);
    const std::string system = requests.at(0).messages.at(0).text();
    EXPECT_THAT(system, HasSubstr("The video is \"Weighing radium\", by Science Hour."));
    EXPECT_THAT(system, HasSubstr("What the whole video says:\nAda weighs radium, sample by sample.\n"));
    EXPECT_THAT(system, HasSubstr("Its speakers:\n- Ada Lovelace, lecturer"));
    EXPECT_THAT(system,
                HasSubstr("Its words for things:\n- NRC\n- half-life: how long it takes to lose half"));
    EXPECT_THAT(system,
                HasSubstr("The article's sections, in order:\n1. [0:00] Weighing radium\n2. [5:00] The "
                          "samples\n3. [10:00] What they mean"));
    for (const llm::ChatRequest& request : requests) {
        EXPECT_EQ(request.messages.at(0).text(), system) << "one system part, for the engine's cache";
    }
    const std::string first = requests.at(0).messages.at(1).text();
    EXPECT_THAT(first,
                HasSubstr("Write section 1, \"Weighing radium\", from 0:00 to 4:55.\n"
                          "\n"
                          "Its paragraphs, in the order told:\n"
                          "<paragraphs>\n"
                          "Paragraph 1: Tells A1 to A3.\n"
                          "A1 [0:30] claim: I'm Ada, and today we weigh radium. Names: Ada.\n"
                          "A2 [1:30] number: The NRC lent us 3 scales for it. Terms: NRC, scales.\n"
                          "A3 [2:00] claim, hedged, aside, high: Maybe the scales are older than the lab.\n"
                          "Paragraph 2:"));
    EXPECT_THAT(first,
                HasSubstr("A6 [4:00] number: In 1902 Marie Curie called radium \"the most precious thing\". "
                          "To quote: \"the most precious thing\". Names: Marie Curie.\n"));
    EXPECT_THAT(first, HasSubstr("A7 [4:30] question: Why does it glow?\n</paragraphs>"))
        << "a question's modality is its kind: said once";
    const std::string third = requests.at(2).messages.at(1).text();
    EXPECT_THAT(third, HasSubstr("Paragraph 1: Tell its atoms in the order given.\n"));
    EXPECT_THAT(third,
                HasSubstr("A26 [14:00] claim, hedged, said by Ada Lovelace, reporting Pierre: I think the "
                          "numbers mean we need better scales, Pierre. Names: Pierre."));
}

TEST_F(WriteStageTest, AFastTierWritesOneDraft) {
    use_tier("fast");
    OracleArmy rig;
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_EQ(article_of(output), oracle_article());
    for (const llm::ChatRequest& request : opened(rig)) {
        EXPECT_FALSE(request.n.has_value()) << "one draft";
    }
    EXPECT_THAT(progress(), ElementsAre("write: the commander writes 3 sections, 1 draft"));
    EXPECT_THAT(rig.requests("judge"), IsEmpty()) << "one draft: nothing to choose between";
}

TEST_F(WriteStageTest, ThePlansDuplicatesAreNotTold) {
    AtomLedger ledger = lecture_ledger();
    ledger.atoms.at(4).text = ledger.atoms.at(3).text;  // A5 says what A4 says
    Plan plan = lecture_plan();
    plan.sections.at(0).paragraphs.at(1).atoms = {AtomId{4}, AtomId{6}};
    plan.duplicates = {knowledge::Duplicate{.atom = AtomId{5}, .of = AtomId{4}, .because = "Again."}};
    OracleArmy rig;
    const StageOutput output = unwrap(run(rig.raise(), plan, ledger));
    EXPECT_THAT(output.findings, IsEmpty()) << "A5 is excused: A4 tells it";
    EXPECT_EQ(article_of(output), oracle_article(plan, ledger));
    EXPECT_THAT(article_of(output), Not(HasSubstr("[A5]")));
}

// --- Checks, and repairs --------------------------------------------------------------------

TEST_F(WriteStageTest, EveryRuleADraftBreaksIsSentBackAndRepaired) {
    struct Case {
        std::string_view why;
        std::function<std::string(const std::string&)> change;
        std::string sent_back;
        std::string_view not_sent{};  // what no message says
    };
    const auto replaced = [](std::string from, std::string to) {
        return [from = std::move(from), to = std::move(to)](const std::string& draft) {
            return replace_all(draft, from, to);
        };
    };
    const std::vector<Case> cases{
        {"a heading", [](const std::string& draft) { return "## Weighing radium\n\n" + draft; },
         "write no heading: the article has its own"},
        {"a list", replaced("Why does it glow? [A7]", "- Why does it glow? [A7]"),
         "write paragraphs of prose only: no lists, no quotes set apart, no code"},
        {"two paragraphs run together", replaced("older than the lab. [A3]\n\n", "older than the lab. [A3] "),
         "write 3 paragraphs, one for each given, not 2", "tell it in paragraph"},
        {"an atom told in another paragraph",
         [](const std::string& draft) {
             return replace_all(replace_all(draft, " Maybe the scales are older than the lab. [A3]", ""),
                                "Its half-life is long. [A4]",
                                "Maybe the scales are older than the lab. [A3] Its half-life is long. [A4]");
         },
         "A3 is told in paragraph 2: tell it in paragraph 1, as given"},
        {"an atom told again in another paragraph",
         replaced("Its half-life is long. [A4]", "Its half-life is long. [A4] Ada weighs radium today. [A1]"),
         "A1 is told again in paragraph 2: tell it in paragraph 1 only"},
        {"a sentence that tells no atom", replaced("for it. [A2]", "for it. [A2] It was a long day."),
         "the sentence \"It was a long day.\" tells no atom: cite the atoms it tells, or leave it out"},
        {"a long one", replaced("for it. [A2]", std::format("for it. [A2] {}", kLongSentence)),
         "the sentence \"The lab was cold that winter, and the old scales creaked each time a sample was "
         "set on their pan by Ada, who sipped caf\u2026\" tells no atom"},
        {"an atom left out", replaced("Its half-life is long. [A4] ", ""),
         "A4 is not retold: tell it, and cite it [A4]"},
        {"a number changed", replaced("lent us 3 scales", "lent us 4 scales"),
         "the paragraph citing A2 lacks \"3\": write it, in digits or words"},
        {"a quote reworded", replaced("the most precious thing", "the most valuable thing"),
         "A6 must be quoted verbatim, \"the most precious thing\""},
        {"another section's atom", replaced("for it. [A2]", "for it. [A2][A8]"),
         "[A8] is not among the atoms to tell here: cite only the atoms given"},
        {"a marker written loosely", replaced("[A1]", "[a1]"), "[a1] is no marker: write it [A1]"},
    };
    for (const Case& test : cases) {
        SCOPED_TRACE(test.why);
        OracleArmy rig{{redrafted("write-1", test.change)}};
        const StageOutput output = unwrap(run(rig.raise()));
        EXPECT_THAT(output.findings, IsEmpty());
        EXPECT_EQ(article_of(output), oracle_article()) << "repaired";
        EXPECT_THAT(sent_to(rig, "write-1"), Contains(HasSubstr(test.sent_back)));
        if (!test.not_sent.empty()) {
            EXPECT_THAT(sent_to(rig, "write-1"), Each(Not(HasSubstr(test.not_sent))));
        }
    }
}

TEST_F(WriteStageTest, OfTheDraftsThatPassTheFirstWithFewestWarningsIsKept) {
    // At a tier without a judge: the first and the last drafts tell A1 after
    // A2, a warning; the second and the third do not, and the third says it
    // in other words.
    use_variant("max-unjudged", [](reason::Tier& tier) { tier.write.judge_top = 0; });
    const auto drafted = [](int number, const std::string& draft) {
        if (number == 1 || number == 4) {
            return reordered(draft);
        }
        return number == 3 ? replace_all(draft, "I'm Ada, and", "I am Ada, and") : draft;
    };
    OracleArmy rig{{on_drafts("write-1", drafted)}, engines({.commander_slots = 1})};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, IsEmpty()) << "the order kept";
    EXPECT_EQ(article_of(output), oracle_article()) << "the second draft";
    EXPECT_EQ(sent_to(rig, "write-1").size(), 4U) << "four drafts, none sent back";
}

TEST_F(WriteStageTest, AWarningNoDraftAvoidsIsPublished) {
    OracleArmy rig{{oracle::edit("reordered", oracle::When{.mission = "write-1"}, reordered)}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, ElementsAre(kReorderedWarning)) << "kept, and said";
    EXPECT_EQ(article_of(output), reordered(oracle_article()));
}

// --- The judge --------------------------------------------------------------------------------

TEST_F(WriteStageTest, TheBestDraftsAreReadByAnotherFamilyAndComparedInBothOrders) {
    OracleArmy rig;
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, IsEmpty());
    EXPECT_EQ(article_of(output), oracle_article()) << "drafts alike: the first is kept";
    EXPECT_THAT(asked(rig, "judge"), ElementsAre("judge-1-draft-1", "judge-1-draft-2", "judge-2-draft-1",
                                                 "judge-2-draft-2", "judge-3-draft-1", "judge-3-draft-2"));
    EXPECT_THAT(asked(rig, "flow"),
                ElementsAre("flow-1-drafts-1-2", "flow-1-drafts-2-1", "flow-2-drafts-1-2",
                            "flow-2-drafts-2-1", "flow-3-drafts-1-2", "flow-3-drafts-2-1"));
    for (const std::string_view kind : {"judge", "flow"}) {
        for (const llm::ChatRequest& request : rig.requests(kind)) {
            EXPECT_EQ(request.model, "gemma-infantry") << "not the writer's family";
            EXPECT_FALSE(request.thinking.value_or(false)) << request.tag.mission_id;
            EXPECT_EQ(request.seed, llm::derived_seed(0, "write", request.tag.mission_id, 0));
        }
    }
}

TEST_F(WriteStageTest, TheJudgeIsShownTheAtomsAndEachSentenceAsWritten) {
    OracleArmy rig;
    unwrap(run(rig.raise()));
    const std::vector<llm::ChatRequest> judges = rig.requests("judge");
    ASSERT_FALSE(judges.empty());
    EXPECT_THAT(judges.front().messages.at(0).text(),
                HasSubstr("The video is \"Weighing radium\", by Science Hour."));
    const std::string task = judges.front().messages.at(1).text();
    EXPECT_THAT(task, HasSubstr("Check section 1, \"Weighing radium\", sentence by sentence.\n\n"
                                "Its atoms:\n"
                                "<atoms>\n"
                                "A1 [0:30] claim: I'm Ada, and today we weigh radium. Names: Ada.\n"));
    EXPECT_THAT(task, HasSubstr("<sentences>\n"
                                "1. I'm Ada, and today we weigh radium. [A1]\n"
                                "2. The NRC lent us 3 scales for it. [A2]\n"));
    const std::vector<llm::ChatRequest> flows = rig.requests("flow");
    ASSERT_FALSE(flows.empty());
    EXPECT_THAT(flows.front().messages.at(1).text(),
                HasSubstr("<first>\nI'm Ada, and today we weigh radium. The NRC lent us 3 scales for it."))
        << "a draft as a reader reads it, without its markers";
}

TEST_F(WriteStageTest, AnIssueTheJudgeReadsInOneDraftKeepsTheOther) {
    const auto drafted = [](int number, const std::string& draft) {
        return number == 2 ? in_other_words(draft) : draft;
    };
    OracleArmy rig{{on_drafts("write-1", drafted),
                    reworded("judge-1-draft-1", read_as(1, "adds", "It adds that the weighing is today."))},
                   engines({.commander_slots = 1})};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, IsEmpty()) << "the draft kept reads clean";
    EXPECT_THAT(article_of(output),
                HasSubstr("## Weighing radium\n\nI am Ada, and today we weigh radium. [A1]"));
}

TEST_F(WriteStageTest, WhatTheJudgeReadsInTheDraftKeptIsPublished) {
    OracleArmy rig{
        {reworded("judge-1-draft-1", read_as(2, "softens", "A2 says the NRC lent them, for sure.")),
         reworded("judge-1-draft-2", read_as(2, "softens", "The same."))}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(
        output.findings,
        ElementsAre("section 1, \"Weighing radium\": the judge reads \"The NRC lent us 3 scales for it. "
                    "[A2]\" as softening what its atoms say: A2 says the NRC lent them, for sure."))
        << "both read so, the first is kept, and its reading said";
}

TEST_F(WriteStageTest, HowTheDraftsReadDecidesBetweenDraftsTheJudgeReadsAlike) {
    // One draft longer by a word than the other: the oracle's flow judge
    // prefers the shorter, in both orders, whichever Ledger B ranks first.
    for (const int longer : {1, 2}) {
        SCOPED_TRACE(std::format("draft {} is the longer", longer));
        const auto drafted = [longer](int number, const std::string& draft) {
            return number == longer ? in_other_words(draft) : draft;
        };
        OracleArmy rig{{on_drafts("write-1", drafted)}, engines({.commander_slots = 1})};
        const StageOutput output = unwrap(run(rig.raise()));
        EXPECT_EQ(article_of(output), oracle_article()) << "the shorter draft, which reads better";
    }
}

TEST_F(WriteStageTest, AJudgeThatFavoursAPositionDecidesNothing) {
    // As above, but shown the longer draft first, the judge prefers the one
    // it sees first, as it does shown them swapped: the orders disagree, a
    // tie, and the first ranked is kept.
    const auto drafted = [](int number, const std::string& draft) {
        return number == 1 ? in_other_words(draft) : draft;
    };
    OracleArmy rig{{on_drafts("write-1", drafted),
                    reworded("flow-1-drafts-1-2", [](json::Json& answer) { answer["better"] = "first"; })},
                   engines({.commander_slots = 1})};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(article_of(output), HasSubstr("I am Ada, and today"));
}

TEST_F(WriteStageTest, AJudgeThatDeclinesLeavesThePickToLedgerBAndSaysSo) {
    OracleArmy rig{{oracle::refusal(oracle::When{.mission = "judge-1-draft-2", .calls = 0}),
                    oracle::refusal(oracle::When{.mission = "flow-2-drafts-2-1", .calls = 0})}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(
        output.findings,
        ElementsAre("section 1, \"Weighing radium\": the judge could not read its drafts: the models "
                    "declined to judge them",
                    "section 2, \"The samples\": the judge could not compare how its drafts read: the "
                    "models declined to compare them"));
    EXPECT_EQ(article_of(output), oracle_article());
    for (const std::string_view kind : {"judge", "flow"}) {
        for (const llm::ChatRequest& request : rig.requests(kind)) {
            EXPECT_EQ(request.model, "gemma-infantry") << "declined, never handed to the writer's family";
        }
    }
}

TEST_F(WriteStageTest, OnlyTheDraftsLedgerBRanksBestAreJudged) {
    // The first two drafts tell A1 after A2, a warning; the last two do not,
    // and only they are judged: the kept one has no warning.
    const auto drafted = [](int number, const std::string& draft) {
        return number <= 2 ? reordered(draft) : draft;
    };
    OracleArmy rig{{on_drafts("write-1", drafted)}, engines({.commander_slots = 1})};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, IsEmpty());
    EXPECT_EQ(article_of(output), oracle_article());
}

TEST_F(WriteStageTest, WhatADraftSaysOutranksTheTalksOrder) {
    // Two drafts (thorough): the first tells A1 after A2, a warning, and the
    // judge reads an issue in the second. What a draft says comes first: the
    // first is kept, and its warning said.
    use_tier("thorough");
    const auto drafted = [](int number, const std::string& draft) {
        return number == 1 ? reordered(draft) : draft;
    };
    OracleArmy rig{{on_drafts("write-1", drafted),
                    reworded("judge-1-draft-1", read_as(1, "adds", "It adds that the weighing is today."))},
                   engines({.commander_slots = 1})};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, ElementsAre(kReorderedWarning)) << "no reading: the draft kept has none";
    EXPECT_EQ(article_of(output), reordered(oracle_article()));
}

TEST_F(WriteStageTest, ThreeDraftsAreComparedPairByPairInBothOrders) {
    // A tier that judges three, their first paragraphs of three lengths: the
    // shortest wins its two comparisons, and is kept. Two orders of length,
    // so that a comparison read for another pair would keep another draft.
    use_variant("max-three-judged", [](reason::Tier& tier) { tier.write.judge_top = 3; });
    const auto shortest = [](const std::string& draft) {
        return replace_all(draft, "I'm Ada, and today", "I'm Ada; today");
    };
    struct Lengths {
        std::string_view order;
        std::function<std::string(int, const std::string&)> drafted;
    };
    const std::vector<Lengths> orders{
        {"the third, the first, the second",
         [&](int number, const std::string& draft) {
             if (number == 3) {
                 return shortest(draft);
             }
             return number == 2 ? in_other_words(draft) : draft;
         }},
        {"the second, the third, the first",
         [&](int number, const std::string& draft) {
             if (number == 2) {
                 return shortest(draft);
             }
             return number == 1 ? in_other_words(draft) : draft;
         }},
    };
    for (const Lengths& lengths : orders) {
        SCOPED_TRACE(lengths.order);
        OracleArmy each{{on_drafts("write-1", lengths.drafted)}, engines({.commander_slots = 1})};
        EXPECT_THAT(article_of(unwrap(run(each.raise()))), HasSubstr("I'm Ada; today we weigh radium. [A1]"));
    }
    OracleArmy rig{{on_drafts("write-1", orders.front().drafted)}, engines({.commander_slots = 1})};
    unwrap(run(rig.raise()));
    std::vector<std::string> first_section;
    for (const std::string& id : asked(rig, "flow")) {
        if (id.starts_with("flow-1-")) {
            first_section.push_back(id);
        }
    }
    EXPECT_THAT(first_section,
                UnorderedElementsAre("flow-1-drafts-1-2", "flow-1-drafts-2-1", "flow-1-drafts-1-3",
                                     "flow-1-drafts-3-1", "flow-1-drafts-2-3", "flow-1-drafts-3-2"))
        << "each pair, in both orders";
}

TEST_F(WriteStageTest, ASingleDraftIsNotJudged) {
    // A tier that asks for a judge, and draws one draft: nothing to choose.
    use_variant("max-one-draft", [](reason::Tier& tier) { tier.write.drafts = 1; });
    OracleArmy rig;
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(rig.requests("judge"), IsEmpty());
    EXPECT_THAT(output.findings, IsEmpty());
}

TEST_F(WriteStageTest, DraftsTheJudgeDoesNotHoldArePickedByLedgerBAlone) {
    // An infantry that holds less than any reading of a section, or any
    // comparison: squad() would refuse the lot, so no section is judged.
    OracleArmy rig{{}, engines({.infantry_holds = 1'024})};
    const StageOutput output = unwrap(run(rig.raise()));
    std::vector<std::string> said;
    for (const SectionPlan& section : lecture_plan().sections) {
        said.push_back(
            std::format("section {}, \"{}\": the drafts are picked by Ledger B alone: they are more "
                        "than the judge holds at once",
                        said.size() + 1, section.heading));
    }
    EXPECT_EQ(output.findings, said);
    EXPECT_THAT(asked(rig, "judge"), IsEmpty());
    EXPECT_THAT(asked(rig, "flow"), IsEmpty());
    EXPECT_EQ(article_of(output), oracle_article()) << "the first drafts, which Ledger B ranks first";
}

TEST_F(WriteStageTest, ASectionInPartsTheJudgeDoesNotHoldIsSaidOnce) {
    // "The samples", in two parts, neither of whose drafts the judge holds.
    OracleArmy rig{{}, engines({.commander_holds = 2'240, .infantry_holds = 1'024})};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(missions_of(rig), ElementsAre("write-1", "write-2-1", "write-2-2", "write-3"));
    std::vector<std::string> said;
    for (const SectionPlan& section : lecture_plan().sections) {
        said.push_back(
            std::format("section {}, \"{}\": the drafts are picked by Ledger B alone: they are more "
                        "than the judge holds at once",
                        said.size() + 1, section.heading));
    }
    EXPECT_EQ(output.findings, said) << "once a section";
}

TEST_F(WriteStageTest, AJudgeWhoseReadingsFailItsChecksIsNotHandedToTheWriter) {
    // The infantry's every reading of the first draft leaves its last
    // sentence unjudged; the commander, of the writer's family, is never asked.
    OracleArmy rig{{oracle::edit(
        "skips a sentence", oracle::When{.mission = "judge-1-draft-1", .model = "gemma-infantry", .calls = 0},
        [](const std::string& answer) {
            json::Json judged = unwrap(json::parse(answer));
            judged.at("sentences").erase(judged.at("sentences").size() - 1);
            return json::dump(judged);
        })}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings,
                ElementsAre("section 1, \"Weighing radium\": the judge could not read its drafts: no "
                            "answer passed its checks (sentence 7 is not judged: judge every "
                            "sentence)"));
    for (const llm::ChatRequest& request : rig.requests("judge")) {
        EXPECT_EQ(request.model, "gemma-infantry") << request.tag.mission_id;
    }
}

TEST_F(WriteStageTest, AJudgeStoppedByACeilingMakesTheResultThisJobsAlone) {
    // A token more than writing takes (the oracle counts four bytes a token,
    // and each section has four drafts alike): the first reading spends past
    // it, and the judge's missions after it, one at a time, stop.
    std::int64_t writing = 0;
    for (const SectionPlan& section : lecture_plan().sections) {
        std::string draft;
        for (const std::string& paragraph : oracle_paragraphs(section, lecture_ledger())) {
            draft += (draft.empty() ? "" : "\n\n") + paragraph;
        }
        writing += 4 * ((static_cast<std::int64_t>(draft.size()) + 3) / 4);
    }
    OracleArmy rig{{}, engines({.infantry_slots = 1}), agents::Ceilings{.completion_tokens = writing + 1}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_FALSE(output.reusable) << "a job allowed more would have judged more";
    EXPECT_THAT(output.findings,
                Contains("section 1, \"Weighing radium\": the judge could not read its drafts: the "
                         "job reached its completion_tokens ceiling first"));
    EXPECT_EQ(article_of(output), oracle_article()) << "Ledger B's pick";
}

TEST_F(WriteStageTest, APodWithoutAJudgeOfAnotherFamilyPicksByLedgerBAndSaysSoOnce) {
    std::vector<agents::Engine> pod = engines();
    pod.at(1).family = "qwen";  // the infantry, of the commander's family
    OracleArmy rig{{}, pod};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(rig.requests("judge"), IsEmpty());
    EXPECT_THAT(output.findings,
                ElementsAre("the drafts are picked by Ledger B alone: the pod has no judge of "
                            "another family than the writer's"));
    EXPECT_EQ(article_of(output), oracle_article());
}

// --- The fusion (Self-MoA) ----------------------------------------------------------------------

// `max`, its drafts fused into one more.
void fusing(reason::Tier& tier) {
    tier.write.fuse = true;
}

// The paragraphs the oracle writes of section `index` (from 0), as one draft.
std::string oracle_draft(std::size_t index) {
    std::string draft;
    for (const std::string& paragraph :
         oracle_paragraphs(lecture_plan().sections.at(index), lecture_ledger())) {
        draft += (draft.empty() ? "" : "\n\n") + paragraph;
    }
    return draft;
}

TEST_F(WriteStageTest, OnlyATierThatFusesAsksForAFusion) {
    OracleArmy rig;
    unwrap(run(rig.raise()));
    EXPECT_THAT(rig.requests("fuse"), IsEmpty()) << "max does not fuse";
}

TEST_F(WriteStageTest, AFusingTierHasTheCommanderWriteOneDraftMoreFromThoseThatPass) {
    use_variant("max-fused", fusing);
    OracleArmy rig;
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, IsEmpty());
    EXPECT_EQ(article_of(output), oracle_article());
    EXPECT_THAT(asked(rig, "fuse"), ElementsAre("fuse-1", "fuse-2", "fuse-3"));
    for (const llm::ChatRequest& request : rig.requests("fuse")) {
        EXPECT_EQ(request.model, "qwen-commander") << "the writer fuses its own drafts";
        EXPECT_FALSE(request.thinking.value_or(false)) << "as the tier writes";
        EXPECT_EQ(request.seed, llm::derived_seed(0, "write", request.tag.mission_id, 0));
        EXPECT_EQ(request.n.value_or(1), 1) << "one draft more";
    }
    // Drafts alike: the fusion, first among them, is judged, and kept.
    EXPECT_THAT(progress(), ElementsAre("write: the commander writes 3 sections, 4 drafts each",
                                        "write: the commander fuses the drafts of 3 sections",
                                        "write: the infantry judges the best 2 drafts of 3 sections",
                                        "write: 3 of 3 fused drafts kept"));
}

TEST_F(WriteStageTest, AFusionThatAvoidsAWarningEveryDraftHasIsKept) {
    // Every draft of the first section tells A1 after A2, a warning; the
    // fusion does not: it ranks first, and the article has no warning.
    use_variant("max-fused", fusing);
    OracleArmy rig{{oracle::edit("reordered", oracle::When{.mission = "write-1", .calls = 0}, reordered)}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, IsEmpty());
    EXPECT_EQ(article_of(output), oracle_article());
    EXPECT_THAT(progress(), Contains("write: 3 of 3 fused drafts kept"));
}

TEST_F(WriteStageTest, TheFusionIsShownTheParagraphsAsTheWriterWasAndTheDraftsBestFirst) {
    // The first and the last drafts tell A1 after A2, a warning: they are
    // shown after the second, and the third, which says A1 in other words.
    use_variant("max-fused", fusing);
    const auto drafted = [](int number, const std::string& draft) {
        if (number == 1 || number == 4) {
            return reordered(draft);
        }
        return number == 3 ? in_other_words(draft) : draft;
    };
    OracleArmy rig{{on_drafts("write-1", drafted)}, engines({.commander_slots = 1})};
    unwrap(run(rig.raise()));
    const std::vector<llm::ChatRequest> fusions = rig.requests("fuse");
    ASSERT_FALSE(fusions.empty());
    ASSERT_EQ(fusions.front().tag.mission_id, "fuse-1");
    const std::string asked = fusions.front().messages.back().text();
    const std::string written = opened(rig).front().messages.back().text();
    const auto paragraphs = [](const std::string& text) {
        const std::size_t begin = text.find("<paragraphs>");
        return text.substr(begin, text.find("</paragraphs>") - begin);
    };
    EXPECT_EQ(paragraphs(asked), paragraphs(written)) << "the paragraphs as their writer was shown them";
    EXPECT_THAT(asked,
                HasSubstr("Write section 1, \"Weighing radium\", from 0:00 to 4:55, taking the best of "
                          "its 4 drafts.\n"));
    const std::string draft = oracle_draft(0);
    EXPECT_THAT(asked,
                HasSubstr(std::format("\n\nIts drafts:\nDraft 1:\n<draft>\n{}\n</draft>\nDraft 2:\n<draft>\n"
                                      "{}\n</draft>\nDraft 3:\n<draft>\n{}\n</draft>\nDraft 4:\n<draft>\n{}\n"
                                      "</draft>",
                                      draft, in_other_words(draft), reordered(draft), reordered(draft))));
}

TEST_F(WriteStageTest, AFusionWithMoreWarningsThanADraftIsNotKept) {
    // Unjudged: the first section's fusion tells A1 after A2, a warning its
    // drafts do not have. It ranks after them, and Ledger B keeps the first.
    use_variant("max-fused-unjudged", [](reason::Tier& tier) {
        tier.write.fuse = true;
        tier.write.judge_top = 0;
    });
    OracleArmy rig{{oracle::edit("reordered", oracle::When{.mission = "fuse-1", .calls = 0}, reordered)}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, IsEmpty());
    EXPECT_EQ(article_of(output), oracle_article());
    EXPECT_THAT(progress(), Contains("write: 2 of 3 fused drafts kept"));
}

TEST_F(WriteStageTest, DraftsTheCommanderDoesNotHoldAllAreFusedBestFirstAsManyAsItHolds) {
    // A commander of 2,264 tokens writes "The samples" in two parts, and holds
    // three of the first section's drafts at once, two of the second part's,
    // and of the first part's, or of the last section's, not two. The first
    // draft drawn tells A1 after A2, a warning: ranked last, it is left out.
    use_variant("max-fused", fusing);
    const auto drafted = [](int number, const std::string& draft) {
        return number == 1 ? reordered(draft) : draft;
    };
    OracleArmy rig{{on_drafts("write-1", drafted)},
                   engines({.commander_holds = 2'264, .commander_slots = 1})};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(missions_of(rig), ElementsAre("write-1", "write-2-1", "write-2-2", "write-3"));
    EXPECT_THAT(asked(rig, "fuse"), ElementsAre("fuse-1", "fuse-2-2"));
    const std::vector<llm::ChatRequest> fusions = rig.requests("fuse");
    ASSERT_FALSE(fusions.empty());
    const std::string shown = fusions.front().messages.back().text();
    EXPECT_THAT(shown, HasSubstr("taking the best of its 3 drafts"));
    EXPECT_THAT(shown, Not(HasSubstr(reordered(oracle_draft(0))))) << "the worst left out";
    EXPECT_THAT(output.findings,
                ElementsAre("section 2, \"The samples\": its drafts are not fused: two of them "
                            "are more than qwen-commander holds at once",
                            "section 3, \"What they mean\": its drafts are not fused: two of "
                            "them are more than qwen-commander holds at once"));
}

TEST_F(WriteStageTest, ACommanderThatHoldsNotEvenTwoDraftsFusesNoneAndSaysSoOnceASection) {
    // 2,000 tokens: every section written, the last two in two parts each,
    // and no two drafts of any part held at once, though one of some is.
    use_variant("max-fused", fusing);
    OracleArmy rig{{}, engines({.commander_holds = 2'000})};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(missions_of(rig), ElementsAre("write-1", "write-2-1", "write-2-2", "write-3-1", "write-3-2"));
    EXPECT_THAT(rig.requests("fuse"), IsEmpty());
    std::vector<std::string> said;
    for (const SectionPlan& section : lecture_plan().sections) {
        said.push_back(
            std::format("section {}, \"{}\": its drafts are not fused: two of them are more than "
                        "qwen-commander holds at once",
                        said.size() + 1, section.heading));
    }
    EXPECT_EQ(output.findings, said) << "once a section, whatever its parts";
    EXPECT_EQ(article_of(output), oracle_article());
}

TEST_F(WriteStageTest, AFusionTheModelsDeclineIsNeverHandedToAnotherFamily) {
    use_variant("max-fused", fusing);
    OracleArmy rig{{oracle::refusal(oracle::When{.mission = "fuse-1", .calls = 0})}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings,
                ElementsAre("section 1, \"Weighing radium\": its drafts could not be fused: the "
                            "models declined to fuse them"));
    for (const llm::ChatRequest& request : rig.requests("fuse")) {
        EXPECT_EQ(request.model, "qwen-commander")
            << request.tag.mission_id << ": a weaker model's would lower them";
    }
    EXPECT_EQ(article_of(output), oracle_article());
}

TEST_F(WriteStageTest, AFusionThatFailsLeavesTheDraftsAsDrawnAndSaysSo) {
    // Every answer of the first section's fusion cites an atom that is none:
    // sent back as a draft is, up to the tier's three rounds, and no rung beyond.
    use_variant("max-fused", fusing);
    OracleArmy rig{{oracle::bad_marker(oracle::When{.mission = "fuse-1", .calls = 0})}};
    const StageOutput output = unwrap(run(rig.raise()));
    ASSERT_EQ(output.findings.size(), 1U);
    EXPECT_THAT(output.findings.front(), HasSubstr("section 1, \"Weighing radium\": its drafts could not be "
                                                   "fused: no answer passed its checks"));
    EXPECT_EQ(article_of(output), oracle_article()) << "the drafts, as drawn";
    std::size_t calls = 0;
    for (const llm::ChatRequest& request : rig.requests("fuse")) {
        if (request.tag.mission_id == "fuse-1") {
            ++calls;
            EXPECT_EQ(request.model, "qwen-commander") << "never another family";
            EXPECT_FALSE(request.thinking.value_or(false)) << "no rung that thinks";
        }
    }
    EXPECT_EQ(calls, 4U) << "the fusion and three repairs";
    EXPECT_THAT(progress(), Contains("write: 2 of 2 fused drafts kept"));
}

TEST_F(WriteStageTest, ADraftThatPassesAloneIsNotFused) {
    // Of the first section's drafts only the first passes: every later answer
    // of its mission cites an atom that is none, repairs and rungs alike.
    use_variant("max-fused", fusing);
    const auto drafted = [](int number, const std::string& draft) {
        return number == 1 ? draft : replace_all(draft, "[A1]", "[A999]");
    };
    OracleArmy rig{{on_drafts("write-1", drafted)}, engines({.commander_slots = 1})};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_EQ(article_of(output), oracle_article()) << "the one that passed";
    EXPECT_THAT(output.findings, IsEmpty()) << "nothing to fuse, and nothing to say";
    EXPECT_THAT(asked(rig, "fuse"), ElementsAre("fuse-2", "fuse-3"));
    EXPECT_THAT(progress(), Contains("write: the commander fuses the drafts of 2 sections"));
}

TEST_F(WriteStageTest, AFusionThinksAsTheTierWrites) {
    use_variant("max-fused-thinking", [](reason::Tier& tier) {
        tier.write.fuse = true;
        tier.write.thinking = true;
    });
    OracleArmy rig;
    unwrap(run(rig.raise()));
    const std::vector<llm::ChatRequest> drafts = rig.requests("write");
    ASSERT_FALSE(drafts.empty());
    ASSERT_TRUE(drafts.front().thinking_budget.has_value());
    const std::vector<llm::ChatRequest> fusions = rig.requests("fuse");
    ASSERT_EQ(fusions.size(), 3U);
    for (const llm::ChatRequest& request : fusions) {
        EXPECT_TRUE(request.thinking.value_or(false)) << request.tag.mission_id;
        EXPECT_EQ(request.thinking_budget, drafts.front().thinking_budget) << "the writer's own budget";
    }
}

TEST_F(WriteStageTest, AFusionStoppedByACeilingMakesTheResultThisJobsAlone) {
    // Unjudged, a token more than writing takes: the first fusion spends past
    // it, and the fusions after it, one at a time, stop.
    use_variant("max-fused-unjudged", [](reason::Tier& tier) {
        tier.write.fuse = true;
        tier.write.judge_top = 0;
    });
    std::int64_t writing = 0;
    for (std::size_t index = 0; index < lecture_plan().sections.size(); ++index) {
        writing += 4 * ((static_cast<std::int64_t>(oracle_draft(index).size()) + 3) / 4);
    }
    OracleArmy rig{{}, engines({.commander_slots = 1}), agents::Ceilings{.completion_tokens = writing + 1}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_FALSE(output.reusable) << "a job allowed more would have fused more";
    EXPECT_THAT(output.findings,
                Contains("section 2, \"The samples\": its drafts could not be fused: the job "
                         "reached its completion_tokens ceiling first"));
    EXPECT_EQ(article_of(output), oracle_article());
}

// --- What no draft passed ---------------------------------------------------------------------

TEST_F(WriteStageTest, ASectionTheModelsDeclineIsToldAsItsAtomsSayIt) {
    AtomLedger ledger = lecture_ledger();
    Atom& eleven = ledger.atoms.at(7);  // A8, said at S11
    eleven.text = "Sample 11 glowed.";
    eleven.must_quote = true;
    eleven.quote = "glowed faintly in the dark";
    OracleArmy rig{{oracle::refusal(oracle::When{.mission = "write-2", .calls = 0})}};
    const StageOutput output = unwrap(run(rig.raise(), lecture_plan(), ledger));
    EXPECT_THAT(output.findings,
                ElementsAre("section 2, \"The samples\": it is told as its atoms say it: the models "
                            "declined to write it"));
    EXPECT_TRUE(output.reusable) << "they would decline again";
    EXPECT_THAT(article_of(output),
                HasSubstr("\n\nSample 11 glowed. (\"glowed faintly in the dark\") [A8] Sample 12 "
                          "glowed faintly"))
        << "each atom's words and its marker, its words to quote kept";
}

TEST_F(WriteStageTest, ASectionWithoutParagraphsIsNotWritten) {
    const AtomLedger fillers = without(lecture_ledger(), {21, 22, 23, 24, 25, 26, 27, 28, 29});
    Plan plan = lecture_plan();
    plan.sections.at(2).paragraphs.clear();
    OracleArmy rig;
    const StageOutput output = unwrap(run(rig.raise(), plan, fillers));
    EXPECT_THAT(missions_of(rig), ElementsAre("write-1", "write-2"));
    EXPECT_THAT(article_of(output), Not(HasSubstr("What they mean"))) << "no heading over nothing";
    EXPECT_THAT(progress(), ElementsAre("write: the commander writes 2 sections, 4 drafts each",
                                        "write: the infantry judges the best 2 drafts of 2 sections"));
}

TEST_F(WriteStageTest, AVideoWithNothingToRetellHasAnEmptyArticleAndSaysSo) {
    std::vector<std::uint32_t> every;
    for (std::uint32_t number = 1; number <= 30; ++number) {
        every.push_back(number);
    }
    Plan plan = lecture_plan();
    for (SectionPlan& section : plan.sections) {
        section.paragraphs.clear();
    }
    OracleArmy rig;
    const StageOutput output = unwrap(run(rig.raise(), plan, without(lecture_ledger(), every)));
    EXPECT_EQ(article_of(output), "");
    EXPECT_THAT(
        output.findings,
        ElementsAre("the article is empty: the video says nothing to retell, every sentence is filler"));
    EXPECT_THAT(missions_of(rig), IsEmpty());
    EXPECT_THAT(progress(), IsEmpty()) << "the commander writes nothing";
}

TEST_F(WriteStageTest, AJobCeilingMakesTheResultThisJobsAlone) {
    OracleArmy rig{{}, engines({.commander_slots = 1}), agents::Ceilings{.completion_tokens = 1}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_FALSE(output.reusable) << "a job allowed more would have written more";
    EXPECT_THAT(output.findings, Contains(HasSubstr("the job reached its completion_tokens ceiling first")));
}

TEST_F(WriteStageTest, WithoutAPodThereIsNoArticle) {
    const Result<StageOutput> output = run({});
    EXPECT_ERRC(output, Errc::needs_user);
    EXPECT_THAT(
        output.error().message(),
        HasSubstr("writing the article needs models, and no pod is configured: rreport doctor says how to "
                  "set one up"));
}

// --- The commander's size, and thinking -----------------------------------------------------

TEST_F(WriteStageTest, ASectionTooLongForTheCommanderIsWrittenInParts) {
    // Its third paragraph's last atom told in the fourth: sent back as the
    // section numbers them.
    const std::string a16 = std::format("{} [A16]", sample(19));
    OracleArmy rig{{redrafted("write-2-2",
                              [&](const std::string& draft) {
                                  return replace_all(replace_all(draft, " " + a16 + "\n\n", "\n\n"),
                                                     "\n\n" + sample(20), "\n\n" + a16 + " " + sample(20));
                              })},
                   engines({.commander_holds = 2'240})};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, IsEmpty()) << "every paragraph written by the commander";
    EXPECT_THAT(missions_of(rig), ElementsAre("write-1", "write-2-1", "write-2-2", "write-3"));
    const std::vector<llm::ChatRequest> requests = opened(rig);
    EXPECT_THAT(
        requests.at(1).messages.at(1).text(),
        HasSubstr("from 5:00 to 9:55.\nIts paragraphs are too many to write at once: write paragraphs 1 to 2 "
                  "here; the others are written apart.\n\nIts paragraphs, in the order told:\n<paragraphs>\n"
                  "Paragraph 1: Tells A8 to A10."));
    EXPECT_THAT(requests.at(2).messages.at(1).text(),
                HasSubstr("<paragraphs>\nParagraph 3: Tells A14 to A16."))
        << "numbered as in the section";
    EXPECT_EQ(article_of(output), oracle_article()) << "the parts' paragraphs joined in order";
    EXPECT_THAT(progress(), ElementsAre("write: the commander writes 3 sections, 4 drafts each, in 4 parts",
                                        "write: the infantry judges the best 2 drafts of 3 sections"));
    EXPECT_THAT(sent_to(rig, "write-2-2"),
                Contains(HasSubstr("A16 is told in paragraph 4: tell it in paragraph 3, as given")))
        << "numbered as in the section";
}

TEST_F(WriteStageTest, APartTheModelsDeclineIsToldAsItsAtomsSayIt) {
    OracleArmy rig{{oracle::refusal(oracle::When{.mission = "write-2-1", .calls = 0})},
                   engines({.commander_holds = 2'240})};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings,
                ElementsAre("section 2, \"The samples\": paragraphs 1 to 2 are told as their "
                            "atoms say it: the models declined to write it"));
    EXPECT_EQ(article_of(output), oracle_article()) << "as the oracle writes them";
}

TEST_F(WriteStageTest, AParagraphTooLongForTheCommanderEvenAloneIsToldAsItsAtomsSayIt) {
    AtomLedger ledger = lecture_ledger();
    std::string endless = "The NRC lent us 3 scales for it";
    while (endless.size() < 6'000) {
        endless += ", and more";
    }
    ledger.atoms.at(1).text = endless + ".";  // A2
    OracleArmy rig{{}, engines({.commander_holds = 4'096})};
    const StageOutput output = unwrap(run(rig.raise(), lecture_plan(), ledger));
    EXPECT_THAT(output.findings,
                ElementsAre("section 1, \"Weighing radium\": paragraph 1 is told as its atoms say "
                            "it: even alone, it is more than qwen-commander holds at once"));
    EXPECT_THAT(missions_of(rig), ElementsAre("write-1-1", "write-2", "write-3"))
        << "the paragraphs after it";
    EXPECT_EQ(article_of(output), oracle_article(lecture_plan(), ledger))
        << "told as the oracle would have told it: its atoms' words, each with its marker";

    // A section of that paragraph alone is none of the commander's.
    Plan alone = lecture_plan();
    alone.sections.at(0).paragraphs = {oracle_paragraph(1, 7)};
    OracleArmy smaller{{}, engines({.commander_holds = 4'096})};
    const StageOutput none = unwrap(run(smaller.raise(), alone, ledger));
    EXPECT_THAT(missions_of(smaller), ElementsAre("write-2", "write-3"));
    EXPECT_THAT(none.findings,
                ElementsAre("section 1, \"Weighing radium\": it is told as its atoms say it: even "
                            "alone, it is more than qwen-commander holds at once"));
    EXPECT_EQ(article_of(none), oracle_article(alone, ledger));
    EXPECT_THAT(progress(), ElementsAre("write: the commander writes 3 sections, 4 drafts each",
                                        "write: the infantry judges the best 2 drafts of 3 sections",
                                        "write: the commander writes 2 sections, 4 drafts each",
                                        "write: the infantry judges the best 2 drafts of 2 sections"))
        << "the sections it was asked for";
}

TEST_F(WriteStageTest, ACommanderThatHoldsNoParagraphEvenAloneTellsEachSectionAsItsAtomsSayIt) {
    OracleArmy rig{{}, engines({.commander_holds = 1'024})};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(missions_of(rig), IsEmpty());
    const auto finding = [](std::string_view section) {
        return std::format(
            "section {}: it is told as its atoms say it: even alone, each paragraph is more than "
            "qwen-commander holds at once",
            section);
    };
    EXPECT_THAT(output.findings, ElementsAre(finding("1, \"Weighing radium\""), finding("2, \"The samples\""),
                                             finding("3, \"What they mean\"")))
        << "one finding a section, not one a paragraph";
    EXPECT_EQ(article_of(output), oracle_article());
    EXPECT_THAT(progress(), IsEmpty()) << "the commander writes nothing";
}

TEST_F(WriteStageTest, AnAnswerMayTakeTwiceItsAtomsWords) {
    OracleArmy rig;
    unwrap(run(rig.raise()));
    const AtomLedger ledger = lecture_ledger();
    std::int64_t bytes = 0;
    for (std::uint32_t number = 8; number <= 17; ++number) {
        bytes += static_cast<std::int64_t>(ledger.atoms.at(number - 1).text.size());
    }
    const std::int64_t paragraphs = 4;
    const std::vector<llm::ChatRequest> requests = opened(rig);
    ASSERT_EQ(requests.size(), 3U);
    EXPECT_EQ(requests.at(1).max_tokens, static_cast<int>(((bytes * 2) / 3) + (paragraphs * 64) + 128))
        << "ten atoms' words, told in twice as many at most; four paragraphs; room besides";
}

TEST_F(WriteStageTest, AnAnswersRoomIsNoLessThan512TokensNorMoreThan8192) {
    AtomLedger ledger = lecture_ledger();
    for (std::size_t index = 0; index < 7; ++index) {  // A1 to A7, the first section: two words each
        Atom& atom = ledger.atoms.at(index);
        atom = Atom{.id = atom.id, .text = "It glowed.", .sentences = atom.sentences};
    }
    std::string endless = "It glowed faintly in the dark";
    while (endless.size() < 4'500) {
        endless += ", and faintly";
    }
    for (std::size_t index = 7; index < 10; ++index) {  // A8 to A10, the second section's first paragraph
        Atom& atom = ledger.atoms.at(index);
        atom = Atom{.id = atom.id, .text = endless + ".", .sentences = atom.sentences};
    }
    OracleArmy rig;
    unwrap(run(rig.raise(), lecture_plan(), ledger));
    const std::vector<llm::ChatRequest> requests = opened(rig);
    ASSERT_EQ(requests.size(), 3U);
    EXPECT_EQ(requests.at(0).max_tokens, 512) << "seven atoms of two words: the least";
    EXPECT_EQ(requests.at(1).max_tokens, 8'192) << "three of 4,500 bytes, and more: the most";
}

TEST_F(WriteStageTest, ATierThatWritesThinkingThinksWithinHalfOfWhatTheCommanderHolds) {
    use_variant("max-thinking", [](reason::Tier& tier) { tier.write.thinking = true; });
    OracleArmy rig{{}, engines({.commander_holds = 16'384, .thinking_budget = 10'000})};
    unwrap(run(rig.raise()));
    ASSERT_THAT(missions_of(rig), ElementsAre("write-1", "write-2", "write-3"));
    for (const llm::ChatRequest& request : opened(rig)) {
        EXPECT_TRUE(request.thinking.value_or(false)) << request.tag.mission_id;
        ASSERT_TRUE(request.thinking_budget.has_value());
        EXPECT_EQ(request.thinking_budget->tokens, 8'192)
            << "the commander's own, within half of what it holds";
    }
}

// --- What is refused, and the key -------------------------------------------------------------

TEST_F(WriteStageTest, APlanThatDoesNotHoldItsLedgerIsABug) {
    Plan plan = lecture_plan();
    plan.sections.at(0).paragraphs.pop_back();  // A7, the question, told nowhere
    OracleArmy rig;
    const Result<StageOutput> output = run(rig.raise(), plan);
    EXPECT_ERRC(output, Errc::internal);
    EXPECT_THAT(output.error().message(), HasSubstr("the plan does not hold its ledger: A7 is neither told"));
}

TEST_F(WriteStageTest, AnOutlineThatDoesNotCoverItsTranscriptIsABug) {
    knowledge::Outline outline = lecture_outline();
    outline.sections.pop_back();
    OracleArmy rig;
    const Result<StageOutput> output = run(rig.raise(), lecture_plan(), lecture_ledger(), outline);
    EXPECT_ERRC(output, Errc::internal);
    EXPECT_THAT(output.error().message(), HasSubstr("the outline does not cover its transcript"));
}

TEST_F(WriteStageTest, ATierRReportDoesNotShipIsNotFound) {
    use_tier("turbo");
    EXPECT_ERRC(run({}), Errc::not_found);
}

TEST_F(WriteStageTest, TheKeyHoldsTheTierThePromptTheArmyAndWhatTheCommanderHolds) {
    OracleArmy rig;
    unwrap(run(rig.raise()));
    const json::Json max = fingerprint();
    EXPECT_EQ(max.at("tier"), unwrap(reason::builtin_tier("max")).canonical(reason::Stage::write));
    const prompt::Library shipped = unwrap(prompt::Library::embedded());
    // The judge's prompt is at its second version: the seams stage's (P20c).
    const std::array<std::pair<std::string_view, std::string_view>, 3> labels{
        {{"write", "write@1"}, {"judge", "judge@2"}, {"flow", "flow@1"}}};
    for (const auto& [name, label] : labels) {
        EXPECT_EQ(max.at("prompts").at(std::string{name}).at("label"), std::string{label});
        EXPECT_EQ(max.at("prompts").at(std::string{name}).at("hash"), unwrap(shipped.content_hash(name)));
    }
    EXPECT_EQ(max.at("holds"), 32'768);
    EXPECT_EQ(max.at("judge_holds"), 32'768)
        << "whether the judge holds the drafts decides whether it reads them";
    EXPECT_EQ(max.at("army").size(), 2U);

    OracleArmy smaller{{}, engines({.commander_holds = 16'384, .infantry_holds = 8'192})};
    unwrap(run(smaller.raise()));
    EXPECT_EQ(fingerprint().at("holds"), 16'384);
    EXPECT_EQ(fingerprint().at("judge_holds"), 8'192);

    use_tier("fast");
    OracleArmy fast;
    unwrap(run(fast.raise()));
    EXPECT_NE(fingerprint().at("tier"), max.at("tier"));
    EXPECT_EQ(fingerprint().at("prompts"), max.at("prompts"));
    EXPECT_FALSE(fingerprint().contains("judge_holds")) << "a tier that does not judge";
    EXPECT_FALSE(max.at("prompts").contains("fuse")) << "a tier that does not fuse never asks its prompt";

    use_variant("max-fused", fusing);
    OracleArmy fused;
    unwrap(run(fused.raise()));
    EXPECT_EQ(fingerprint().at("prompts").at("fuse").at("label"), "fuse@1");
    EXPECT_EQ(fingerprint().at("prompts").at("fuse").at("hash"), unwrap(shipped.content_hash("fuse")));
    EXPECT_EQ(fingerprint().at("prompts").at("write"), max.at("prompts").at("write"));
}

}  // namespace
}  // namespace rr::pipeline

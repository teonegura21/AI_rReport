// The write stage on the synthetic lecture (lecture.hpp), its ledger and the
// plan the oracle makes of them: every section written by the commander,
// every atom told and cited; every rule a draft breaks sent back; what no
// draft passed told as its atoms say it, and said; with the faults real
// models make injected where a test says.
#include <rr/pipeline/write_stage.hpp>

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
using knowledge::Paragraph;
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
using testing::oracle_paragraph;
using testing::OracleArmy;
using testing::source;
using testing::without;
using testkit::unwrap;

// --- What the article should be -------------------------------------------------------------

// The article the oracle writes of `plan` (rr/oracle/answers.hpp): each
// section under its heading, each paragraph its atoms' words, each followed
// by its marker.
std::string oracle_article(const Plan& plan = lecture_plan(), const AtomLedger& ledger = lecture_ledger()) {
    std::string markdown;
    for (const SectionPlan& section : plan.sections) {
        if (section.paragraphs.empty()) {
            continue;
        }
        markdown += std::format("{}## {}\n", markdown.empty() ? "" : "\n", section.heading);
        for (const Paragraph& paragraph : section.paragraphs) {
            std::string told;
            for (const AtomId id : paragraph.atoms) {
                told += std::format("{}{} [{}]", told.empty() ? "" : " ",
                                    ledger.atoms.at(id.number() - 1).text, id);
            }
            markdown += "\n" + told + "\n";
        }
    }
    return markdown;
}

// A sentence of 129 bytes whose "\u00e9" spans its 120th and 121st: one a
// message shows cut, before that character.
constexpr std::string_view kLongSentence =
    "The lab was cold that winter, and the old scales creaked each time a sample was set on their pan by "
    "Ada, "
    "who sipped caf\u00e9 slowly.";

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
    EXPECT_THAT(progress(), ElementsAre("write: the commander writes 3 sections, 4 drafts each"));
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
        {"a sentence that tells no atom", replaced("for it. [A2]", "for it. [A2] It was a long day."),
         "the sentence \"It was a long day.\" tells no atom: cite the atoms it tells, or leave it out"},
        {"a long one", replaced("for it. [A2]", std::format("for it. [A2] {}", kLongSentence)),
         "the sentence \"The lab was cold that winter, and the old scales creaked each time a sample was set "
         "on "
         "their pan by Ada, who sipped caf\u2026\" tells no atom"},
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
    // The first and the last drafts tell A1 after A2, a warning; the second
    // and the third do not, and the third says it in other words.
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
    EXPECT_THAT(progress(), ElementsAre("write: the commander writes 2 sections, 4 drafts each"));
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
    EXPECT_THAT(progress(), ElementsAre("write: the commander writes 3 sections, 4 drafts each, in 4 parts"));
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
                                        "write: the commander writes 2 sections, 4 drafts each"))
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
    EXPECT_EQ(max.at("prompts").at("write").at("label"), "write@1");
    EXPECT_EQ(max.at("prompts").at("write").at("hash"),
              unwrap(unwrap(prompt::Library::embedded()).content_hash("write")));
    EXPECT_EQ(max.at("holds"), 32'768);
    EXPECT_EQ(max.at("army").size(), 2U);

    OracleArmy smaller{{}, engines({.commander_holds = 16'384})};
    unwrap(run(smaller.raise()));
    EXPECT_EQ(fingerprint().at("holds"), 16'384);

    use_tier("fast");
    OracleArmy fast;
    unwrap(run(fast.raise()));
    EXPECT_NE(fingerprint().at("tier"), max.at("tier"));
    EXPECT_EQ(fingerprint().at("prompts"), max.at("prompts"));
}

}  // namespace
}  // namespace rr::pipeline

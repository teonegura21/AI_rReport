// The seams stage on the synthetic lecture (lecture.hpp): the article the
// oracle writes of its plan, each seam rewritten by the commander, checked as
// a draft is, and read again by the judge where the tier rechecks. The
// oracle rewrites a seam as written; a test that wants another rewrite, or
// another reading, changes the answer (a fault).
#include <rr/pipeline/seams_stage.hpp>

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
#include <rr/knowledge/plan.hpp>
#include <rr/knowledge/transcript.hpp>
#include <rr/llm/memo.hpp>
#include <rr/oracle/oracle.hpp>
#include <rr/pipeline/drafting.hpp>
#include <rr/pipeline/source.hpp>
#include <rr/reason/tier.hpp>
#include <rr/testkit/result.hpp>

#include "lecture.hpp"
#include "oracle_army.hpp"

namespace rr::pipeline {
namespace {

using knowledge::AtomLedger;
using knowledge::Plan;
using ::testing::AllOf;
using ::testing::Contains;
using ::testing::ElementsAre;
using testing::engines;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using testing::lecture;
using testing::lecture_ledger;
using testing::lecture_outline;
using testing::lecture_plan;
using testing::oracle_article;
using testing::oracle_paragraphs;
using testing::OracleArmy;
using testing::reworded;
using testing::source;
using testkit::unwrap;

// The lecture's two seams, as the oracle writes its article: section 1 ends
// on A7, a paragraph of its own, and section 2 opens with A8 to A10; section
// 2 ends on A17, and section 3 opens with A18 to A20.
constexpr std::string_view kFirstSeam =
    R"(the seam between section 1, "Weighing radium", and section 2, "The samples")";
constexpr std::string_view kSecondSeam =
    R"(the seam between section 2, "The samples", and section 3, "What they mean")";

// The paragraph that ends section 1, as the oracle writes it.
constexpr std::string_view kEndOfFirst = "Why does it glow? [A7]";

// The lecture's atom `number` as the oracle tells it: its words, then its marker.
std::string told(std::uint32_t number) {
    return std::format("{} [A{}]", lecture_ledger().atoms.at(number - 1).text, number);
}

// A link that says nothing new: "So why does it glow? [A7]".
std::string so(const std::string& text) {
    return replace_all(text, kEndOfFirst, std::format("So {}", "why does it glow? [A7]"));
}

// The second paragraph of a seam's answer opened with "Then: ", a link that
// says nothing new.
std::string then(const std::string& answer) {
    return replace_all(answer, "\n\n", "\n\nThen: ");
}

// A fault that changes the answer to the first `calls` calls of `mission`
// (0: every one) as `change` says.
oracle::Fault rewritten(std::string mission, std::function<std::string(const std::string&)> change,
                        int calls = 1) {
    return oracle::edit("rewritten", oracle::When{.mission = std::move(mission), .calls = calls},
                        [change = std::move(change)](const std::string& answer) { return change(answer); });
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

// The last message of every request of `mission`, of `kind`, in the order sent.
std::vector<std::string> sent_to(const OracleArmy& rig, std::string_view kind, std::string_view mission) {
    std::vector<std::string> messages;
    for (const llm::ChatRequest& request : rig.requests(kind)) {
        if (request.tag.mission_id == mission) {
            messages.push_back(request.messages.back().text());
        }
    }
    return messages;
}

// The lines a writer is shown of atoms `first` to `last` (atom_entry()).
std::string entries(std::uint32_t first, std::uint32_t last) {
    const knowledge::Transcript transcript = lecture();
    const AtomLedger ledger = lecture_ledger();
    std::string lines;
    for (std::uint32_t number = first; number <= last; ++number) {
        lines += (lines.empty() ? "" : "\n") + atom_entry(ledger.atoms.at(number - 1), transcript);
    }
    return lines;
}

class SeamsStageTest : public ::testing::Test {
protected:
    Result<StageOutput> run(RaiseArmy army, const std::string& article = oracle_article(),
                            const Plan& plan = lecture_plan(),
                            const knowledge::Outline& outline = lecture_outline()) {
        StageContext context{.job = &job_,
                             .army = std::move(army),
                             .prompts = &prompts_,
                             .progress = [this](std::string_view line) { progress_.emplace_back(line); }};
        context.inputs.emplace("source", to_json(source()));
        context.inputs.emplace("transcript", knowledge::to_json(lecture()));
        context.inputs.emplace("outline", knowledge::to_json(outline));
        context.inputs.emplace("atomize", knowledge::to_json(lecture_ledger()));
        context.inputs.emplace("plan", knowledge::to_json(plan));
        context.inputs.emplace("write", knowledge::to_json(knowledge::Article{.markdown = article}));
        RR_TRY_ASSIGN(context.fingerprint, stage_.fingerprint(context));
        fingerprint_ = context.fingerprint;
        return stage_.run(context);
    }

    static std::string article_of(const StageOutput& output) {
        return unwrap(knowledge::article_from_json(output.artifact)).markdown;
    }

    void use_tier(std::string tier) { job_.tier = std::move(tier); }

    [[nodiscard]] const json::Json& fingerprint() const { return fingerprint_; }
    [[nodiscard]] const std::vector<std::string>& progress() const { return progress_; }

private:
    library::Job job_{.id = "J1", .tier = "max"};
    const prompt::Library prompts_ = unwrap(prompt::Library::embedded());
    const SeamsStage stage_;
    json::Json fingerprint_;
    std::vector<std::string> progress_;
};

// --- Each seam rewritten ---------------------------------------------------------------------

TEST_F(SeamsStageTest, EachSeamIsRewrittenByTheCommanderAndOneAsWrittenIsNotReadAgain) {
    OracleArmy rig;
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, IsEmpty());
    EXPECT_TRUE(output.reusable);
    EXPECT_EQ(article_of(output), oracle_article()) << "the oracle rewrites a seam as written";
    EXPECT_THAT(asked(rig, "seam"), ElementsAre("seam-1", "seam-2")) << "a mission a seam";
    for (const llm::ChatRequest& request : rig.requests("seam")) {
        EXPECT_EQ(request.model, "qwen-commander");
        EXPECT_FALSE(request.thinking.value_or(false)) << "rewriting prose does not think";
        EXPECT_EQ(request.seed, llm::derived_seed(0, "seams", request.tag.mission_id, 0)) << "each its own";
    }
    EXPECT_THAT(rig.requests("judge"), IsEmpty()) << "a seam rewritten as written has nothing to choose";
    EXPECT_THAT(progress(), ElementsAre("seams: the commander rewrites 2 seams"));
}

TEST_F(SeamsStageTest, TheCommanderIsShownBothParagraphsAndTheAtomsEachCites) {
    OracleArmy rig;
    unwrap(run(rig.raise()));
    const std::vector<llm::ChatRequest> requests = rig.requests("seam");
    ASSERT_EQ(requests.size(), 2U);
    const std::string first = requests.front().messages.back().text();
    EXPECT_THAT(first, HasSubstr(std::format("Smooth {}.", kFirstSeam)));
    EXPECT_THAT(first,
                HasSubstr(std::format("Paragraph 1, the end of section 1. Its atoms:\n<atoms>\n{}\n</atoms>\n"
                                      "As written:\n<first>\n{}\n</first>",
                                      entries(7, 7), kEndOfFirst)));
    EXPECT_THAT(first, HasSubstr(std::format(
                           "Paragraph 2, the start of section 2. Its atoms:\n<atoms>\n{}\n</atoms>\n"
                           "As written:\n<second>\n{}\n</second>",
                           entries(8, 10),
                           oracle_paragraphs(lecture_plan().sections.at(1), lecture_ledger()).front())));
    const std::string second = requests.back().messages.back().text();
    EXPECT_THAT(second, HasSubstr(std::format("Smooth {}.", kSecondSeam)));
    EXPECT_THAT(second, HasSubstr(std::format("<atoms>\n{}\n</atoms>\nAs written:\n<first>\n{}\n</first>",
                                              entries(17, 17), told(17))));
    EXPECT_THAT(second,
                HasSubstr(std::format("<atoms>\n{}\n</atoms>\nAs written:\n<second>\n{} {} {}\n</second>",
                                      entries(18, 20), told(18), told(19), told(20))));
    EXPECT_THAT(requests.front().messages.front().text(),
                HasSubstr(std::format("The video is \"{}\"", source().video.title)));
    EXPECT_EQ(requests.front().messages.front().text(), requests.back().messages.front().text())
        << "what the seams share is in the system part, which the engine's cache keeps";
}

TEST_F(SeamsStageTest, ARewriteIsKeptWhereTheJudgeReadsNoWorse) {
    OracleArmy rig{{rewritten("seam-1", so)}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, IsEmpty());
    EXPECT_EQ(article_of(output), so(oracle_article()));
    EXPECT_THAT(asked(rig, "judge"), ElementsAre("judge-seam-1-rewritten", "judge-seam-1-written"))
        << "both read, by the judge of another family";
    for (const llm::ChatRequest& request : rig.requests("judge")) {
        EXPECT_EQ(request.model, "gemma-infantry") << request.tag.mission_id;
        EXPECT_THAT(request.messages.back().text(),
                    HasSubstr(std::format("Check {}, sentence by sentence.", kFirstSeam)));
    }
    EXPECT_THAT(
        sent_to(rig, "judge", "judge-seam-1-rewritten"),
        ElementsAre(HasSubstr(std::format("<sentences>\n1. So why does it glow? [A7]\n2. {}\n", told(8)))));
    EXPECT_THAT(sent_to(rig, "judge", "judge-seam-1-written"),
                ElementsAre(HasSubstr(std::format("<sentences>\n1. {}\n2. {}\n", kEndOfFirst, told(8)))));
    EXPECT_THAT(progress(), ElementsAre("seams: the commander rewrites 2 seams",
                                        "seams: the infantry reads 1 seam, as written and as rewritten"));
}

TEST_F(SeamsStageTest, ARewriteTheJudgeReadsWorseStaysAsWrittenAndItIsSaid) {
    OracleArmy rig{{rewritten("seam-1", so), reworded("judge-seam-1-rewritten",
                                                      read_as(1, "adds", "It adds a cause no atom gives."))}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(
        output.findings,
        ElementsAre(std::format(
            "{} stays as written: the judge reads more issues in its rewrite (1 against 0)", kFirstSeam)));
    EXPECT_EQ(article_of(output), oracle_article());
}

TEST_F(SeamsStageTest, ARewriteTheJudgeReadsNoWorseIsKeptAndWhatItReadsIsSaid) {
    // The judge reads an issue in both: a tie keeps the rewrite, and its
    // reading of it is published.
    OracleArmy rig{{rewritten("seam-1", so),
                    reworded("judge-seam-1-rewritten", read_as(1, "adds", "It adds."), 0),
                    reworded("judge-seam-1-written", read_as(2, "softens", "It softens."), 0)}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(
        output.findings,
        ElementsAre(std::format("{}: the judge reads \"So why does it glow? [A7]\" as adding what its atoms "
                                "do not say: It adds.",
                                kFirstSeam)));
    EXPECT_EQ(article_of(output), so(oracle_article()));
}

// --- Checks, and repairs --------------------------------------------------------------------

TEST_F(SeamsStageTest, EveryRuleARewriteBreaksIsSentBackAndRepaired) {
    struct Case {
        std::string_view why;
        std::function<std::string(const std::string&)> change;
        std::string sent_back;
    };
    const auto replaced = [](std::string from, std::string to) {
        return [from = std::move(from), to = std::move(to)](const std::string& answer) {
            return replace_all(answer, from, to);
        };
    };
    const std::vector<Case> cases{
        {"a heading", replaced("[A7]\n\n", "[A7]\n\n## The samples\n\n"),
         "write no heading: the article has its own"},
        {"one paragraph", replaced("[A7]\n\n", "[A7] "), "write 2 paragraphs, one for each given, not 1"},
        {"an atom moved across the seam",
         [](const std::string& answer) {
             return replace_all(replace_all(answer, told(8) + " ", ""), std::string{kEndOfFirst},
                                std::format("{} {}", kEndOfFirst, told(8)));
         },
         "A8 is told in paragraph 1: tell it in paragraph 2, as given"},
        {"an atom told again across the seam", replaced("[A10]", std::format("[A10] {}", kEndOfFirst)),
         "A7 is told again in paragraph 2: tell it in paragraph 1 only"},
        {"a sentence that tells no atom", replaced("[A7]", "[A7] Here is why."),
         "the sentence \"Here is why.\" tells no atom: cite the atoms it tells, or leave it out"},
        {"an atom left out", replaced(told(9) + " ", ""), "A9 is not retold: tell it, and cite it [A9]"},
        {"another section's atom", replaced("[A7]", "[A7][A2]"),
         "[A2] is not among the atoms to tell here: cite only the atoms given"},
    };
    for (const Case& test : cases) {
        SCOPED_TRACE(test.why);
        OracleArmy rig{{rewritten("seam-1", test.change)}};
        const StageOutput output = unwrap(run(rig.raise()));
        EXPECT_THAT(output.findings, IsEmpty());
        EXPECT_EQ(article_of(output), oracle_article()) << "repaired: as written";
        EXPECT_THAT(sent_to(rig, "seam", "seam-1"), Contains(HasSubstr(test.sent_back)));
    }
}

TEST_F(SeamsStageTest, ASeamNoRewritePassesStaysAsWrittenAndItIsSaid) {
    OracleArmy rig{{rewritten(
        "seam-1", [](const std::string& answer) { return replace_all(answer, told(9) + " ", ""); }, 0)}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings,
                ElementsAre(std::format("{} stays as written: no answer passed its checks (A9 is "
                                        "not retold: tell it, and cite it [A9])",
                                        kFirstSeam)));
    EXPECT_EQ(article_of(output), oracle_article());
}

TEST_F(SeamsStageTest, ASeamTheModelsDeclineStaysAsWrittenAndNoOtherFamilyWritesIt) {
    OracleArmy rig{{oracle::refusal(oracle::When{.mission = "seam-1", .calls = 0})}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, ElementsAre(std::format(
                                     "{} stays as written: the models declined to rewrite it", kFirstSeam)));
    EXPECT_EQ(article_of(output), oracle_article());
    for (const llm::ChatRequest& request : rig.requests("seam")) {
        EXPECT_EQ(request.model, "qwen-commander") << "the judge's family never writes what it reads";
    }
}

TEST_F(SeamsStageTest, ASeamTheCommanderDoesNotHoldStaysAsWritten) {
    OracleArmy rig{{}, engines({.commander_holds = 1'024})};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(
        output.findings,
        ElementsAre(std::format("{} stays as written: even its two paragraphs are more than qwen-commander "
                                "holds at once",
                                kFirstSeam),
                    std::format("{} stays as written: even its two paragraphs are more than qwen-commander "
                                "holds at once",
                                kSecondSeam)));
    EXPECT_THAT(rig.requests("seam"), IsEmpty());
    EXPECT_THAT(progress(), IsEmpty());
}

TEST_F(SeamsStageTest, AJobCeilingMakesTheResultThisJobsAlone) {
    OracleArmy rig{{}, engines({.commander_slots = 1}), agents::Ceilings{.completion_tokens = 1}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_FALSE(output.reusable) << "a job allowed more would have rewritten more";
    EXPECT_THAT(output.findings,
                Contains(HasSubstr("stays as written: the job reached its completion_tokens ceiling "
                                   "first")));
}

// --- Without the judge ------------------------------------------------------------------------

TEST_F(SeamsStageTest, AJudgeThatCannotReadASeamLeavesItToLedgerB) {
    OracleArmy rig{
        {rewritten("seam-1", so),
         oracle::edit("skips a sentence", oracle::When{.mission = "judge-seam-1-rewritten", .calls = 0},
                      [](const std::string& answer) {
                          json::Json judged = unwrap(json::parse(answer));
                          judged.at("sentences").erase(judged.at("sentences").size() - 1);
                          return json::dump(judged);
                      })}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(
        output.findings,
        ElementsAre(std::format("{}: Ledger B alone decides on its rewrite: the judge could not read it: no "
                                "answer passed its checks (sentence 4 is not judged: judge every sentence)",
                                kFirstSeam)));
    EXPECT_EQ(article_of(output), so(oracle_article())) << "no more warnings than as written";
    for (const llm::ChatRequest& request : rig.requests("judge")) {
        EXPECT_EQ(request.model, "gemma-infantry") << "the commander, who rewrote it, never judges it";
    }
}

TEST_F(SeamsStageTest, AJudgeStoppedByACeilingMakesTheResultThisJobsAlone) {
    // A token more than the two rewrites take (the oracle counts four bytes a
    // token): the judge's first reading spends past it, and its second, one
    // at a time, stops.
    const std::vector<knowledge::SectionPlan>& sections = lecture_plan().sections;
    const auto tokens = [](const std::string& text) {
        return (static_cast<std::int64_t>(text.size()) + 3) / 4;
    };
    const std::int64_t rewriting =
        tokens(so(std::format("{}\n\n{}", kEndOfFirst,
                              oracle_paragraphs(sections.at(1), lecture_ledger()).front()))) +
        tokens(
            std::format("{}\n\n{}", told(17), oracle_paragraphs(sections.at(2), lecture_ledger()).front()));
    OracleArmy rig{{rewritten("seam-1", so)},
                   engines({.infantry_slots = 1}),
                   agents::Ceilings{.completion_tokens = rewriting + 1}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_FALSE(output.reusable) << "a job allowed more would have read more";
    EXPECT_THAT(
        output.findings,
        ElementsAre(std::format("{}: Ledger B alone decides on its rewrite: the judge could not read it: the "
                                "job reached its completion_tokens ceiling first",
                                kFirstSeam)));
    EXPECT_EQ(article_of(output), so(oracle_article()));
}

TEST_F(SeamsStageTest, AJudgeThatDoesNotHoldASeamLeavesItToLedgerB) {
    OracleArmy rig{{rewritten("seam-1", so)}, engines({.infantry_holds = 1'024})};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(
        output.findings,
        ElementsAre(std::format("{}: Ledger B alone decides on its rewrite: it is more than the judge holds "
                                "at once",
                                kFirstSeam)));
    EXPECT_THAT(rig.requests("judge"), IsEmpty());
    EXPECT_EQ(article_of(output), so(oracle_article()));
}

TEST_F(SeamsStageTest, WithoutAJudgeOfAnotherFamilyLedgerBDecidesAndItIsSaidOnce) {
    std::vector<agents::Engine> pod = engines();
    pod.at(1).family = "qwen";  // the infantry, of the commander's family
    OracleArmy rig{{rewritten("seam-1", so), rewritten("seam-2", then)}, pod};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings,
                ElementsAre("the seams' rewrites are kept by Ledger B alone: the pod has no judge "
                            "of another family than the writer's"));
    EXPECT_THAT(rig.requests("judge"), IsEmpty());
    EXPECT_EQ(article_of(output), replace_all(so(oracle_article()), "\n" + told(18), "\nThen: " + told(18)));
}

// --- Ledger B --------------------------------------------------------------------------------

TEST_F(SeamsStageTest, ARewriteLedgerBWarnsOfMoreInStaysAsWritten) {
    // At thorough, which rewrites without the judge: A9 told before A8.
    use_tier("thorough");
    OracleArmy rig{{rewritten("seam-1", [](const std::string& answer) {
        return replace_all(answer, told(8) + " " + told(9), told(9) + " " + told(8));
    })}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings,
                ElementsAre(std::format(
                    "{} stays as written: Ledger B warns of more in its rewrite (1 against 0)", kFirstSeam)));
    EXPECT_EQ(article_of(output), oracle_article());
    EXPECT_THAT(rig.requests("judge"), IsEmpty());
}

TEST_F(SeamsStageTest, WhatLedgerBFindsInTheArticleThatItDidNotFindAsWrittenIsSaid) {
    // As written, A9 is told before A8, and A12 before A11: warnings the
    // write stage said. The rewrite tells A8 first, then A10 before A9: as
    // many warnings, so it is kept, and the article's new one, of A10, is
    // said. A12's, in a paragraph no seam touches, is not said again.
    const std::string written =
        replace_all(replace_all(oracle_article(), told(8) + " " + told(9), told(9) + " " + told(8)),
                    told(11) + " " + told(12), told(12) + " " + told(11));
    OracleArmy rig{{rewritten("seam-1", [](const std::string& answer) {
        return replace_all(answer, told(9) + " " + told(8) + " " + told(10),
                           told(8) + " " + told(10) + " " + told(9));
    })}};
    const StageOutput output = unwrap(run(rig.raise(), written));
    EXPECT_THAT(output.findings,
                ElementsAre("A10 was said at S13, before what this section tells ahead of it: keep "
                            "the order of the talk"));
    EXPECT_THAT(article_of(output), HasSubstr(told(8) + " " + told(10) + " " + told(9)));
}

// --- Where the seams are -----------------------------------------------------------------------

TEST_F(SeamsStageTest, SeamsThatShareAParagraphAreRewrittenOneAfterTheOther) {
    // "The samples" told in one paragraph: it ends the first seam and opens
    // the second, which is shown it as the first left it.
    Plan plan = lecture_plan();
    plan.sections.at(1).paragraphs = {testing::oracle_paragraph(8, 17)};
    OracleArmy rig{{rewritten("seam-1", then)}};
    const StageOutput output = unwrap(run(rig.raise(), oracle_article(plan), plan));
    EXPECT_THAT(output.findings, IsEmpty());
    EXPECT_THAT(article_of(output), HasSubstr("\nThen: " + told(8)));
    EXPECT_THAT(sent_to(rig, "seam", "seam-2"), ElementsAre(HasSubstr("<first>\nThen: " + told(8))));
    EXPECT_THAT(progress(), ElementsAre("seams: the commander rewrites 1 seam",
                                        "seams: the infantry reads 1 seam, as written and as rewritten",
                                        "seams: the commander rewrites 1 more seam"));
}

TEST_F(SeamsStageTest, AChainOfOneParagraphSectionsAlternatesBetweenTheWaves) {
    // Four sections, the middle two of one paragraph each: the second seam
    // shares a paragraph with the first and with the third, so it waits for
    // both, and is shown both as they left them.
    knowledge::Outline outline = lecture_outline();
    outline.sections.at(2).last = knowledge::SentenceId{25};
    outline.sections.push_back(knowledge::Section{
        .first = knowledge::SentenceId{26}, .last = knowledge::SentenceId{30}, .title = "What is left"});
    Plan plan = lecture_plan();
    plan.sections.at(1).paragraphs = {testing::oracle_paragraph(8, 17)};
    plan.sections.at(2).paragraphs = {testing::oracle_paragraph(18, 22)};
    plan.sections.push_back(knowledge::SectionPlan{.first = knowledge::SentenceId{26},
                                                   .heading = "What is left",
                                                   .paragraphs = {testing::oracle_paragraph(23, 26)},
                                                   .planned_by = knowledge::PlannedBy::commander});
    OracleArmy rig{{rewritten("seam-1", then), rewritten("seam-3", [](const std::string& answer) {
                        return replace_all(answer, told(18), "Thus: " + told(18));
                    })}};
    const StageOutput output = unwrap(run(rig.raise(), oracle_article(plan), plan, outline));
    EXPECT_THAT(output.findings, IsEmpty());
    EXPECT_THAT(asked(rig, "seam"), ElementsAre("seam-1", "seam-2", "seam-3"));
    EXPECT_THAT(sent_to(rig, "seam", "seam-2"), ElementsAre(AllOf(HasSubstr("<first>\nThen: " + told(8)),
                                                                  HasSubstr("<second>\nThus: " + told(18)))));
    EXPECT_THAT(progress(), ElementsAre("seams: the commander rewrites 2 seams",
                                        "seams: the infantry reads 2 seams, as written and as rewritten",
                                        "seams: the commander rewrites 1 more seam"));
    EXPECT_THAT(article_of(output), AllOf(HasSubstr("Then: " + told(8)), HasSubstr("Thus: " + told(18))));
}

TEST_F(SeamsStageTest, AHeadingThatCitesIsNoParagraph) {
    // A heading is a model's free text: one that holds a marker is no
    // paragraph of its section.
    Plan plan = lecture_plan();
    plan.sections.at(1).heading = "The samples [A9]";
    OracleArmy rig;
    const StageOutput output = unwrap(run(rig.raise(), oracle_article(plan), plan));
    EXPECT_THAT(
        sent_to(rig, "seam", "seam-1"),
        ElementsAre(HasSubstr(std::format("<second>\n{} {} {}\n</second>", told(8), told(9), told(10)))));
    EXPECT_EQ(article_of(output), oracle_article(plan));
}

TEST_F(SeamsStageTest, AnAtomCitedTwiceInAParagraphIsShownOnce) {
    const std::string written = replace_all(oracle_article(), std::string{kEndOfFirst},
                                            "Why does it glow? [A7] Nobody knows yet. [A7]");
    OracleArmy rig;
    unwrap(run(rig.raise(), written));
    EXPECT_THAT(sent_to(rig, "seam", "seam-1"),
                ElementsAre(HasSubstr(
                    std::format("Its atoms:\n<atoms>\n{}\n</atoms>\nAs written:\n<first>", entries(7, 7)))));
}

TEST_F(SeamsStageTest, APlanThatDoesNotHoldItsLedgerIsABug) {
    Plan plan = lecture_plan();
    plan.sections.at(0).paragraphs.pop_back();  // A7, the question, told nowhere
    OracleArmy rig;
    const Result<StageOutput> output = run(rig.raise(), oracle_article(), plan);
    EXPECT_ERRC(output, Errc::internal);
    EXPECT_THAT(output.error().message(), HasSubstr("the plan does not hold its ledger: A7 is neither told"));
}

TEST_F(SeamsStageTest, AnArticleOfOneSectionHasNoSeam) {
    Plan first = lecture_plan();
    first.sections.erase(first.sections.begin() + 1, first.sections.end());
    OracleArmy rig;
    const StageOutput output = unwrap(run(rig.raise(), oracle_article(first)));
    EXPECT_THAT(output.findings, IsEmpty());
    EXPECT_EQ(article_of(output), oracle_article(first));
    EXPECT_THAT(rig.requests("seam"), IsEmpty());
    EXPECT_THAT(progress(), IsEmpty());
}

TEST_F(SeamsStageTest, AnArticleThatCitesWhatItsPlanDoesNotTellIsABug) {
    OracleArmy rig;
    const Result<StageOutput> output =
        run(rig.raise(), replace_all(oracle_article(), std::string{kEndOfFirst}, "Why does it glow? [A27]"));
    EXPECT_ERRC(output, Errc::internal);
    EXPECT_THAT(output.error().message(),
                HasSubstr("the article is not its plan's: it cites A27, which no section of its plan tells"));

    OracleArmy second;
    const Result<StageOutput> later =
        run(second.raise(),
            replace_all(oracle_article(), std::string{kEndOfFirst}, "Why does it glow? [A7][A27]"));
    EXPECT_ERRC(later, Errc::internal) << "a marker after the first too";
}

// --- Tiers, and the key ------------------------------------------------------------------------

TEST_F(SeamsStageTest, AtThoroughARewriteIsKeptUnread) {
    use_tier("thorough");
    OracleArmy rig{{rewritten("seam-1", so)}};
    const StageOutput output = unwrap(run(rig.raise()));
    EXPECT_THAT(output.findings, IsEmpty());
    EXPECT_EQ(article_of(output), so(oracle_article()));
    EXPECT_THAT(rig.requests("judge"), IsEmpty());
    EXPECT_THAT(progress(), ElementsAre("seams: the commander rewrites 2 seams"));
}

TEST_F(SeamsStageTest, AtFastTheArticleIsAsWrittenAndNoModelIsAsked) {
    use_tier("fast");
    const StageOutput output = unwrap(run({}));
    EXPECT_THAT(output.findings, IsEmpty());
    EXPECT_EQ(article_of(output), oracle_article());
    EXPECT_EQ(fingerprint(),
              (json::Json{{"tier", unwrap(reason::builtin_tier("fast")).canonical(reason::Stage::seams)}}))
        << "no model: neither prompts nor army in its key";
    EXPECT_THAT(progress(), IsEmpty());
}

TEST_F(SeamsStageTest, WithoutAPodATierThatRewritesNeedsYou) {
    const Result<StageOutput> output = run({});
    EXPECT_ERRC(output, Errc::needs_user);
    EXPECT_THAT(
        output.error().message(),
        HasSubstr("smoothing the article's seams needs models, and no pod is configured: rreport doctor says "
                  "how to set one up"));
}

TEST_F(SeamsStageTest, TheKeyHoldsTheModeAndWithModelsThePromptsTheArmyAndWhatEachHolds) {
    OracleArmy rig;
    unwrap(run(rig.raise()));
    const json::Json max = fingerprint();
    EXPECT_EQ(max.at("tier"), unwrap(reason::builtin_tier("max")).canonical(reason::Stage::seams));
    const prompt::Library shipped = unwrap(prompt::Library::embedded());
    EXPECT_EQ(max.at("prompts").at("seam").at("label"), "seam@1");
    EXPECT_EQ(max.at("prompts").at("seam").at("hash"), unwrap(shipped.content_hash("seam")));
    EXPECT_EQ(max.at("prompts").at("judge").at("label"), "judge@2");
    EXPECT_EQ(max.at("prompts").at("judge").at("hash"), unwrap(shipped.content_hash("judge")));
    EXPECT_EQ(max.at("army").size(), 2U);
    EXPECT_EQ(max.at("holds"), 32'768);
    EXPECT_EQ(max.at("judge_holds"), 32'768);

    OracleArmy smaller{{}, engines({.commander_holds = 16'384, .infantry_holds = 8'192})};
    unwrap(run(smaller.raise()));
    EXPECT_EQ(fingerprint().at("holds"), 16'384);
    EXPECT_EQ(fingerprint().at("judge_holds"), 8'192);

    use_tier("thorough");
    OracleArmy thorough;
    unwrap(run(thorough.raise()));
    EXPECT_NE(fingerprint().at("tier"), max.at("tier"));
    EXPECT_FALSE(fingerprint().at("prompts").contains("judge")) << "a tier that does not recheck";
    EXPECT_FALSE(fingerprint().contains("judge_holds"));
}

}  // namespace
}  // namespace rr::pipeline

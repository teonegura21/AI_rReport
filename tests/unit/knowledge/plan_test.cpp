#include <rr/knowledge/plan.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/core/error.hpp>
#include <rr/testkit/result.hpp>

namespace rr::knowledge {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;
using testkit::unwrap;

// Six atoms, A1 said at S1 to A6 at S6.
AtomLedger six_atoms() {
    AtomLedger ledger;
    for (std::uint32_t number = 1; number <= 6; ++number) {
        ledger.atoms.push_back(
            Atom{.id = AtomId{number}, .text = "Something.", .sentences = {SentenceId{number}}});
    }
    return ledger;
}

// Two sections, S1–S3 and S4–S6.
Outline two_sections() {
    return Outline{.sections = {Section{.first = SentenceId{1}, .last = SentenceId{3}, .title = "Polonium"},
                                Section{.first = SentenceId{4}, .last = SentenceId{6}, .title = "Radium"}}};
}

// The six atoms planned: A1–A3 told in the first section, a paragraph
// grouped without a purpose among them; A4 and A5 in the second, and A6 a
// duplicate of A5.
Plan planned() {
    return Plan{
        .sections = {SectionPlan{.first = SentenceId{1},
                                 .heading = "Finding polonium",
                                 .paragraphs = {Paragraph{.purpose = "How it was found.",
                                                          .atoms = {AtomId{1}, AtomId{2}}},
                                                Paragraph{.atoms = {AtomId{3}}}},
                                 .planned_by = PlannedBy::commander},
                     SectionPlan{.first = SentenceId{4},
                                 .heading = "Radium",
                                 .paragraphs = {Paragraph{.atoms = {AtomId{4}, AtomId{5}}}},
                                 .planned_by = PlannedBy::grouping}},
        .duplicates = {Duplicate{.atom = AtomId{6}, .of = AtomId{5}, .because = "It says A5 again."}}};
}

void expect_refused(const json::Json& document, std::string_view because) {
    const Result<Plan> read = plan_from_json(document);
    ASSERT_FALSE(read.has_value()) << because;
    EXPECT_EQ(read.error().code(), Errc::invalid_argument);
    EXPECT_NE(read.error().message().find(because), std::string::npos) << read.error().message();
}

TEST(Plan, RoundTripsThroughItsArtifact) {
    const Plan plan = planned();
    const json::Json document = to_json(plan);
    EXPECT_EQ(document.at("sections").at(0).at("paragraphs").at(1).at("purpose"), nullptr)
        << "a purpose no model said is null, never empty";
    EXPECT_EQ(document.at("sections").at(0).at("planned_by"), "commander");
    EXPECT_EQ(document.at("sections").at(1).at("planned_by"), "grouping");
    EXPECT_EQ(unwrap(plan_from_json(document)), plan);

    Plan unexplained = planned();
    unexplained.duplicates.at(0).because.reset();
    EXPECT_EQ(to_json(unexplained).at("duplicates").at(0).at("because"), nullptr);
    EXPECT_EQ(unwrap(plan_from_json(to_json(unexplained))), unexplained);
}

TEST(Plan, NamesWhoPlannedASection) {
    EXPECT_EQ(to_string(PlannedBy::commander), "commander");
    EXPECT_EQ(to_string(PlannedBy::grouping), "grouping");
}

TEST(Plan, ASectionWhoseSentencesAreAllFillerHasNoParagraph) {
    Plan plan = planned();
    plan.sections.push_back(SectionPlan{.first = SentenceId{7}, .heading = "Thanks"});
    EXPECT_EQ(unwrap(plan_from_json(to_json(plan))), plan);
}

TEST(Plan, SectionsRunInTheOrderSaidEachOnce) {
    json::Json repeated = to_json(planned());
    repeated["sections"][1]["first"] = "S1";
    expect_refused(repeated,
                   "the section at S1 comes after the one at S1: sections run in the order said, each once");

    json::Json back = to_json(planned());
    back["sections"].push_back(json::Json{{"first", "S2"},
                                          {"heading", "Back"},
                                          {"paragraphs", json::Json::array()},
                                          {"planned_by", "grouping"}});
    expect_refused(back, "the section at S2 comes after the one at S4");
}

TEST(Plan, AnAtomIsToldOnceOrDeclaredADuplicateNeverBoth) {
    json::Json twice = to_json(planned());
    twice["sections"][1]["paragraphs"][0]["atoms"].push_back("A1");
    expect_refused(twice, "A1 is told twice: an atom is told once, in one paragraph");

    json::Json both = to_json(planned());
    both["duplicates"][0]["atom"] = "A4";
    expect_refused(both, "A4 is told and declared a duplicate: it is one or the other");

    json::Json itself = to_json(planned());
    itself["duplicates"][0]["of"] = "A6";
    expect_refused(itself, "A6 is declared a duplicate of itself");

    json::Json again = to_json(planned());
    again["duplicates"].push_back(again["duplicates"][0]);
    expect_refused(again, "A6 is declared a duplicate twice");

    json::Json untold = to_json(planned());
    untold["duplicates"][0]["of"] = "A9";
    expect_refused(untold, "A6 is declared a duplicate of A9, which no paragraph tells");

    json::Json chained = to_json(planned());
    chained["duplicates"].push_back(json::Json{{"atom", "A7"}, {"of", "A6"}, {"because", nullptr}});
    expect_refused(chained, "A7 is declared a duplicate of A6, which no paragraph tells");
}

TEST(Plan, TheSchemaIsStrict) {
    std::vector<std::pair<std::string_view, json::Json>> cases;
    cases.emplace_back("an unknown field", to_json(planned()));
    cases.back().second["sections"][0]["summary"] = "radium";
    cases.emplace_back("a paragraph without atoms", to_json(planned()));
    cases.back().second["sections"][0]["paragraphs"][1]["atoms"] = json::Json::array();
    cases.emplace_back("an atom written loosely", to_json(planned()));
    cases.back().second["sections"][0]["paragraphs"][1]["atoms"][0] = "a3";
    cases.emplace_back("an empty purpose", to_json(planned()));
    cases.back().second["sections"][0]["paragraphs"][0]["purpose"] = "";
    cases.emplace_back("an empty heading", to_json(planned()));
    cases.back().second["sections"][0]["heading"] = "";
    cases.emplace_back("an unknown planner", to_json(planned()));
    cases.back().second["sections"][0]["planned_by"] = "infantry";
    cases.emplace_back("no duplicates", to_json(planned()));
    cases.back().second.erase("duplicates");
    for (const auto& [why, document] : cases) {
        EXPECT_ERRC(plan_from_json(document), Errc::invalid_argument) << why;
    }
}

TEST(PlanProblems, NoneWhenThePlanHoldsTheLedgerAsTheOutlineCutsIt) {
    EXPECT_THAT(plan_problems(planned(), six_atoms(), two_sections()), IsEmpty());
}

TEST(PlanProblems, SayWhatToCorrect) {
    Plan unplanned = planned();
    unplanned.sections.pop_back();
    unplanned.duplicates.clear();
    EXPECT_THAT(
        plan_problems(unplanned, six_atoms(), two_sections()),
        ElementsAre("the outline's section at S4, \"Radium\", is not planned: plan every section of "
                    "the outline",
                    "A4 is neither told nor declared a duplicate: every atom of the ledger reaches the "
                    "article",
                    "A5 is neither told nor declared a duplicate: every atom of the ledger reaches the "
                    "article",
                    "A6 is neither told nor declared a duplicate: every atom of the ledger reaches the "
                    "article"));

    Plan extra = planned();
    extra.sections.push_back(SectionPlan{.first = SentenceId{7}, .heading = "Thanks"});
    EXPECT_THAT(plan_problems(extra, six_atoms(), two_sections()),
                ElementsAre("the plan has a section at S7, which the outline has not: plan the outline's "
                            "sections only"));

    Plan elsewhere = planned();
    std::swap(elsewhere.sections[0].paragraphs[1].atoms[0], elsewhere.sections[1].paragraphs[0].atoms[0]);
    EXPECT_THAT(
        plan_problems(elsewhere, six_atoms(), two_sections()),
        ElementsAre("A4, said at S4, is told in the section at S1: tell it in the section at S4, where "
                    "it was said",
                    "A3, said at S3, is told in the section at S4: tell it in the section at S1, where "
                    "it was said"));

    Plan unknown = planned();
    unknown.sections[1].paragraphs[0].atoms.emplace_back(9);
    unknown.duplicates.push_back(Duplicate{.atom = AtomId{8}, .of = AtomId{9}});
    EXPECT_THAT(plan_problems(unknown, six_atoms(), two_sections()),
                ElementsAre("A9 is told in the section at S4, and the ledger has no A9",
                            "A8 is declared a duplicate, and the ledger has no A8"));
}

}  // namespace
}  // namespace rr::knowledge

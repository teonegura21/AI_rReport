// What a judge is shown of a draft, and how its answers are checked and
// read (rr/pipeline/judging.hpp).
#include <rr/pipeline/judging.hpp>

#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace rr::pipeline {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

// A judgment of sentences numbered as `numbers` say, every one "none".
json::Json judged(const std::vector<int>& numbers) {
    json::Json sentences = json::Json::array();
    for (const int number : numbers) {
        sentences.push_back(json::Json{
            {"sentence", number}, {"evidence", "A1: \"x\""}, {"reading", nullptr}, {"issue", "none"}});
    }
    return json::Json{{"sentences", std::move(sentences)}};
}

TEST(Judging, AJudgeIsShownEachSentenceOfTheParagraphsAsWritten) {
    EXPECT_THAT(sentences_of(
                    "## A heading\n\nWater boils at 100 degrees [A1]. Ice melts. [A2]\n\nIt was hot [A3].\n"),
                ElementsAre("Water boils at 100 degrees [A1].", "Ice melts. [A2]", "It was hot [A3]."))
        << "a heading is no sentence to judge";
    EXPECT_THAT(sentences_of(""), IsEmpty());
}

TEST(Judging, EverySentenceIsJudgedOnceInOrder) {
    EXPECT_THAT(judgment_problems(judged({1, 2, 3}), 3), IsEmpty());
    EXPECT_THAT(
        judgment_problems(judged({1, 3, 2}), 3),
        ElementsAre("sentence 3 is judged where sentence 2 should be: judge each sentence once, in order",
                    "sentence 2 is judged where sentence 3 should be: judge each sentence once, in order"));
    EXPECT_THAT(judgment_problems(judged({1, 2, 4}), 3),
                ElementsAre("there is no sentence 4: the section has 3"));
    EXPECT_THAT(judgment_problems(judged({1}), 3),
                ElementsAre("sentences 2 to 3 are not judged: judge every sentence"));
    EXPECT_THAT(judgment_problems(judged({1, 2}), 3),
                ElementsAre("sentence 3 is not judged: judge every sentence"));
}

TEST(Judging, AJudgmentIsReadAsItWasGiven) {
    json::Json answer = judged({1, 2});
    answer["sentences"][1]["issue"] = "softens";
    answer["sentences"][1]["reading"] = "It hedges what A2 says for sure.";
    EXPECT_THAT(judgment_of(answer), ElementsAre(Judged{.sentence = 1, .evidence = "A1: \"x\""},
                                                 Judged{.sentence = 2,
                                                        .evidence = "A1: \"x\"",
                                                        .reading = "It hedges what A2 says for sure.",
                                                        .issue = Issue::softens}));
}

TEST(Judging, IssuesHaveStableNamesAndReadings) {
    for (const Issue issue : {Issue::none, Issue::adds, Issue::softens, Issue::intensifies,
                              Issue::misattributes, Issue::contradicts}) {
        EXPECT_EQ(issue_from_string(to_string(issue)), issue);
    }
    EXPECT_EQ(issue_from_string("embellishes"), std::nullopt);
    EXPECT_EQ(reading_of(Issue::adds), "adding what its atoms do not say");
}

TEST(Judging, AFlowJudgesPreferenceIsRead) {
    EXPECT_EQ(preference_of(json::Json{{"reasons", "r"}, {"better", "first"}}), reason::Preference::first);
    EXPECT_EQ(preference_of(json::Json{{"reasons", "r"}, {"better", "second"}}), reason::Preference::second);
    EXPECT_EQ(preference_of(json::Json{{"reasons", "r"}, {"better", "tie"}}), reason::Preference::tie);
}

}  // namespace
}  // namespace rr::pipeline

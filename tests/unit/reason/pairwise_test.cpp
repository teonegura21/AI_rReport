#include <rr/reason/pairwise.hpp>

#include <gtest/gtest.h>

namespace rr::reason {
namespace {

TEST(Pairwise, AWinMustSurviveTheSwap) {
    EXPECT_EQ(swapped(Preference::first, Preference::second), Winner::a)
        << "a first, then a second: a both times";
    EXPECT_EQ(swapped(Preference::second, Preference::first), Winner::b);
    EXPECT_EQ(swapped(Preference::first, Preference::first), Winner::tie)
        << "whatever came first: position bias";
    EXPECT_EQ(swapped(Preference::second, Preference::second), Winner::tie);
    EXPECT_EQ(swapped(Preference::first, Preference::tie), Winner::tie);
    EXPECT_EQ(swapped(Preference::tie, Preference::tie), Winner::tie);
}

TEST(Pairwise, ATallyCountsWinsTiesAndTheJudgesInconsistency) {
    PairwiseTally tally;
    EXPECT_DOUBLE_EQ(tally.score_a(), 0.5) << "nothing judged, nothing known";
    tally.add(Preference::first, Preference::second);  // a
    tally.add(Preference::second, Preference::first);  // b
    tally.add(Preference::first, Preference::first);   // tie, the orders disagreeing
    tally.add(Preference::tie, Preference::tie);       // tie, both orders agreeing
    EXPECT_EQ(tally.a, 1);
    EXPECT_EQ(tally.b, 1);
    EXPECT_EQ(tally.ties, 2);
    EXPECT_EQ(tally.inconsistent, 1);
    EXPECT_EQ(tally.pairs(), 4);
    EXPECT_DOUBLE_EQ(tally.score_a(), 0.5);

    PairwiseTally clear;
    clear.add(Preference::first, Preference::second);
    clear.add(Preference::first, Preference::second);
    clear.add(Preference::tie, Preference::tie);
    EXPECT_DOUBLE_EQ(clear.score_a(), 2.5 / 3.0) << "a tie counts half";
}

TEST(Pairwise, WinnersHaveNames) {
    EXPECT_EQ(to_string(Winner::a), "a");
    EXPECT_EQ(to_string(Winner::b), "b");
    EXPECT_EQ(to_string(Winner::tie), "tie");
}

}  // namespace
}  // namespace rr::reason

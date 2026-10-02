// Whether the eval's judges read as a person does: Wilson's interval and
// Cohen's κ against published values, each class's bars, and the biases.
#include <rr/eval/calibration.hpp>

#include <array>
#include <string_view>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace rr::eval {
namespace {

using ::testing::DoubleNear;

constexpr double kClose = 5e-5;  // the published values' four decimals

void expect_interval(const std::optional<Interval>& interval, double low, double high) {
    ASSERT_TRUE(interval.has_value());
    EXPECT_THAT(interval->low, DoubleNear(low, kClose));
    EXPECT_THAT(interval->high, DoubleNear(high, kClose));
}

// Wilson (1927); the values as Newcombe (1998) tabulates them, and as
// Python computes them independently.
TEST(Wilson, IsTheScoreIntervalAtNinetyFivePercent) {
    expect_interval(wilson(5, 10), 0.2366, 0.7634);
    expect_interval(wilson(17, 20), 0.6396, 0.9476);
    expect_interval(wilson(0, 10), 0.0, 0.2775);   // never below 0, where the normal interval falls
    expect_interval(wilson(10, 10), 0.7225, 1.0);  // never above 1
    expect_interval(wilson(1, 1), 0.2065, 1.0);
    EXPECT_EQ(wilson(0, 0), std::nullopt) << "no trial, no interval";
}

constexpr std::array<std::string_view, 2> kTwo{"yes", "no"};

// Verdicts making up a table, rows the person's, columns the judge's.
std::vector<Verdicts> table(const std::vector<std::vector<std::size_t>>& counts) {
    std::vector<Verdicts> verdicts;
    for (std::size_t person = 0; person < counts.size(); ++person) {
        for (std::size_t judge = 0; judge < counts[person].size(); ++judge) {
            for (std::size_t item = 0; item < counts[person][judge]; ++item) {
                verdicts.push_back(Verdicts{.person = person, .judge = judge});
            }
        }
    }
    return verdicts;
}

// Cohen (1960): agreement beyond chance.
TEST(Agree, IsCohensKappaOverTheKindsClasses) {
    const JudgeAgreement agreement = agree("infantry", kTwo, table({{20, 5}, {10, 15}}), 3);
    EXPECT_EQ(agreement.judge, "infantry");
    EXPECT_EQ(agreement.items, 50U);
    EXPECT_EQ(agreement.unread, 3U) << "counted, and left out";
    EXPECT_THAT(*agreement.observed, DoubleNear(0.7, 1e-12)) << "35 of 50";
    EXPECT_THAT(*agreement.chance, DoubleNear(0.5, 1e-12)) << "0.5 × 0.6 + 0.5 × 0.4";
    EXPECT_THAT(*agreement.kappa, DoubleNear(0.4, 1e-12)) << "(0.7 − 0.5) / (1 − 0.5)";

    const JudgeAgreement perfect = agree("j", kTwo, table({{7, 0}, {0, 3}}), 0);
    EXPECT_THAT(*perfect.kappa, DoubleNear(1.0, 1e-12));
    const JudgeAgreement opposite = agree("j", kTwo, table({{0, 5}, {5, 0}}), 0);
    EXPECT_THAT(*opposite.kappa, DoubleNear(-1.0, 1e-12));
}

TEST(Agree, SaysNothingOfOneClassOnlyNorOfNoItem) {
    const JudgeAgreement one_class = agree("j", kTwo, table({{12, 0}, {0, 0}}), 0);
    EXPECT_THAT(*one_class.observed, DoubleNear(1.0, 1e-12));
    EXPECT_EQ(one_class.kappa, std::nullopt) << "chance agreement is 1: κ is 0 / 0";
    EXPECT_FALSE(one_class.passes()) << "agreement on one class says nothing of the judge";

    const JudgeAgreement none = agree("j", kTwo, {}, 4);
    EXPECT_EQ(none.items, 0U);
    EXPECT_EQ(none.observed, std::nullopt);
    EXPECT_EQ(none.kappa, std::nullopt);
    EXPECT_FALSE(none.passes());
}

// A class's bars are a random judge's: precision, the class's share of the
// person's verdicts; recall, the judge's own rate of saying it.
TEST(Agree, EachClassClearsTheBarsOfARandomJudgeWeightedAsThisOneIs) {
    const JudgeAgreement agreement = agree("j", kTwo, table({{17, 3}, {2, 18}}), 0);
    ASSERT_EQ(agreement.classes.size(), 2U);
    const ClassAgreement& yes = agreement.classes[0];
    EXPECT_EQ(yes.name, "yes");
    EXPECT_EQ(yes.person, 20U);
    EXPECT_EQ(yes.judge, 19U);
    EXPECT_EQ(yes.both, 17U);
    expect_interval(yes.precision, 0.6861, 0.9706);  // 17 of 19
    expect_interval(yes.recall, 0.6396, 0.9476);     // 17 of 20
    EXPECT_THAT(yes.precision_bar, DoubleNear(0.5, 1e-12)) << "20 of 40 the person's";
    EXPECT_THAT(yes.recall_bar, DoubleNear(19.0 / 40, 1e-12)) << "19 of 40 the judge's";
    EXPECT_TRUE(yes.clears());
    EXPECT_TRUE(agreement.passes()) << "κ 0.75, and both classes clear";

    // Agreeing more often than not is not enough on few items: 4 of 5 has a
    // lower bound of 0.38, under a bar of a half.
    const JudgeAgreement few = agree("j", kTwo, table({{4, 1}, {1, 4}}), 0);
    EXPECT_THAT(*few.kappa, DoubleNear(0.6, 1e-12));
    EXPECT_FALSE(few.classes[0].clears());
    EXPECT_FALSE(few.passes()) << "κ clears 0.4, and the intervals are too wide";

    // Each bar alone keeps a class back. A judge that says it rarely, and
    // rightly, still misses most of it: precision clears (1 of 1, its lower
    // bound 0.21 over 0.2), recall does not (1 of 2, 0.09 under 0.1). And
    // the other way round.
    const JudgeAgreement rare = agree("j", kTwo, table({{1, 1}, {0, 8}}), 0);
    EXPECT_GT(rare.classes[0].precision->low, rare.classes[0].precision_bar);
    EXPECT_LE(rare.classes[0].recall->low, rare.classes[0].recall_bar);
    EXPECT_FALSE(rare.classes[0].clears());
    const JudgeAgreement eager = agree("j", kTwo, table({{1, 0}, {1, 8}}), 0);
    EXPECT_LE(eager.classes[0].precision->low, eager.classes[0].precision_bar);
    EXPECT_GT(eager.classes[0].recall->low, eager.classes[0].recall_bar);
    EXPECT_FALSE(eager.classes[0].clears());
}

TEST(Agree, AClassOnlyOneOfThemUsedCannotClear) {
    constexpr std::array<std::string_view, 3> kThree{"told", "partly", "not_told"};
    // The judge says "partly" twice, where the person never does; neither
    // says "not_told", which is then not checked at all.
    const JudgeAgreement agreement = agree("j", kThree, table({{18, 2, 0}, {0, 0, 0}, {0, 0, 0}}), 0);
    ASSERT_EQ(agreement.classes.size(), 2U) << "told and partly; not_told used by neither";
    const ClassAgreement& partly = agreement.classes[1];
    EXPECT_EQ(partly.name, "partly");
    EXPECT_EQ(partly.recall, std::nullopt) << "the person never said it: no recall";
    expect_interval(partly.precision, 0.0, 0.6576);  // 0 of 2
    EXPECT_FALSE(partly.clears());
    EXPECT_FALSE(agreement.passes());
}

TEST(Agree, NeedsKappaAtTheBarEvenWithEveryClassClear) {
    // 62 items, κ 0.35, every class's bounds above its bars: the smallest
    // such table there is.
    const JudgeAgreement under = agree("j", kTwo, table({{19, 0}, {23, 20}}), 0);
    EXPECT_THAT(*under.kappa, DoubleNear(0.3477, kClose));
    EXPECT_TRUE(under.classes[0].clears() && under.classes[1].clears());
    EXPECT_FALSE(under.passes());

    // κ exactly 0.4, which floating point computes a hair under (2/9 over
    // 5/9 is 0.39999999999999997): at the bar is enough.
    const JudgeAgreement at = agree("j", kTwo, table({{16, 0}, {16, 16}}), 0);
    EXPECT_LT(*at.kappa, kKappaBar);
    EXPECT_TRUE(at.classes[0].clears() && at.classes[1].clears());
    EXPECT_TRUE(at.passes());
}

TEST(LengthsDiffer, AtAFifthOrMore) {
    EXPECT_TRUE(lengths_differ(80, 100)) << "the shorter four fifths of the longer";
    EXPECT_TRUE(lengths_differ(100, 80)) << "either way round";
    EXPECT_FALSE(lengths_differ(81, 100));
    EXPECT_FALSE(lengths_differ(0, 0)) << "two empty versions are alike";
    EXPECT_TRUE(lengths_differ(0, 1));
}

TEST(Biases, CountFlipsAndPreferencesForTheLongerVersion) {
    std::vector<OrderedReading> readings;
    readings.push_back(OrderedReading{.flipped = true});
    readings.push_back(OrderedReading{.longer = true});
    readings.push_back(OrderedReading{.longer = true});
    readings.push_back(OrderedReading{.longer = false});
    readings.push_back(OrderedReading{});  // a tie, or versions alike in length
    const Biases found = biases(readings);
    EXPECT_EQ(found.read, 5U);
    EXPECT_EQ(found.flipped, 1U);
    EXPECT_EQ(found.flip_rate, wilson(1, 5));
    EXPECT_EQ(found.unequal, 3U);
    EXPECT_EQ(found.longer, 2U);
    EXPECT_EQ(found.longer_rate, wilson(2, 3));

    const Biases none = biases({});
    EXPECT_EQ(none.read, 0U);
    EXPECT_EQ(none.flip_rate, std::nullopt);
}

}  // namespace
}  // namespace rr::eval

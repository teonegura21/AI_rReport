#include <rr/text/numbers.hpp>

#include <cmath>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace rr::text {
namespace {

// A mention as a test states it: its value, its unit, and the text it covers.
struct Seen {
    double value = 0.0;
    std::string text;
    bool percent = false;
    bool ordinal = false;

    friend bool operator==(const Seen& lhs, const Seen& rhs) {
        return same_value(lhs.value, rhs.value) && lhs.text == rhs.text && lhs.percent == rhs.percent &&
               lhs.ordinal == rhs.ordinal;
    }
    friend void PrintTo(const Seen& seen, std::ostream* out) {
        *out << seen.value << (seen.percent ? "%" : "") << (seen.ordinal ? " (ordinal)" : "") << " \""
             << seen.text << '"';
    }
};

std::vector<Seen> read(std::string_view text) {
    std::vector<Seen> found;
    for (const NumberMention& mention : numbers(text)) {
        found.push_back(Seen{.value = mention.value,
                             .text = std::string{text_of(text, mention.span)},
                             .percent = mention.percent,
                             .ordinal = mention.ordinal});
    }
    return found;
}

Seen percent(double value, std::string text) {
    return Seen{.value = value, .text = std::move(text), .percent = true};
}

Seen ordinal(double value, std::string text) {
    return Seen{.value = value, .text = std::move(text), .ordinal = true};
}

using All = std::vector<Seen>;

TEST(Numbers, DigitsAreReadByValue) {
    EXPECT_EQ(read("37 people, 1,234,567 views, 3.14 and 0.5"),
              (All{{37, "37"}, {1234567, "1,234,567"}, {3.14, "3.14"}, {0.5, "0.5"}}));
}

TEST(Numbers, KnowWhereTheyAreAndHowTheyAreWritten) {
    const std::vector<NumberMention> found = numbers("Up 37 or thirty-seven or 1.5 million");
    ASSERT_EQ(found.size(), 3U);
    EXPECT_EQ(found[0].span, (Span{.begin = 3, .end = 5}));
    EXPECT_EQ(found[0].form, NumberForm::digits);
    EXPECT_EQ(found[1].form, NumberForm::words);
    EXPECT_EQ(found[2].form, NumberForm::mixed);
}

TEST(Numbers, APointThatStartsANumberIsADecimalPoint) {
    EXPECT_EQ(read("costs .5 percent; -.5 and $.50"),
              (All{percent(0.5, ".5 percent"), {-0.5, "-.5"}, {0.5, ".50"}}));
    EXPECT_EQ(read("the end.5 more"), (All{{5, "5"}})) << "a point after a word ends the sentence";
}

TEST(Numbers, DigitsOfAnyScriptAreRead) {
    EXPECT_EQ(read("\u0663\u0664 \uFF13\uFF14 \u0969\u096A"),
              (All{{34, "\u0663\u0664"}, {34, "\uFF13\uFF14"}, {34, "\u0969\u096A"}}));
}

TEST(Numbers, NumberWordsAreRead) {
    EXPECT_EQ(read("zero, seven, thirteen, thirty-seven, forty two, ninety-nine"),
              (All{{0, "zero"},
                   {7, "seven"},
                   {13, "thirteen"},
                   {37, "thirty-seven"},
                   {42, "forty two"},
                   {99, "ninety-nine"}}));
}

TEST(Numbers, HundredsAndScalesMultiply) {
    EXPECT_EQ(read("one hundred and five, twelve hundred, two thousand and eight, a hundred and twenty"),
              (All{{105, "one hundred and five"},
                   {1200, "twelve hundred"},
                   {2008, "two thousand and eight"},
                   {120, "a hundred and twenty"}}));
    EXPECT_EQ(read("two million three hundred thousand, a dozen, two dozen, a million"),
              (All{{2300000, "two million three hundred thousand"},
                   {12, "a dozen"},
                   {24, "two dozen"},
                   {1e6, "a million"}}));
}

TEST(Numbers, AScaleFollowsDigitsToo) {
    EXPECT_EQ(read("1.5 million, 5 thousand, 999 trillion, 2 dozen"),
              (All{{1.5e6, "1.5 million"}, {5000, "5 thousand"}, {999e12, "999 trillion"}, {24, "2 dozen"}}));
}

TEST(Numbers, FractionsOfAWholeAreRead) {
    EXPECT_EQ(read("three point one four, two point oh five, two and a half million, a million and a half"),
              (All{{3.14, "three point one four"},
                   {2.05, "two point oh five"},
                   {2.5e6, "two and a half million"},
                   {1.5e6, "a million and a half"}}));
}

TEST(Numbers, ASpokenDecimalHasTheValueOfItsDigits) {
    const std::vector<NumberMention> spoken = numbers("three point one four");
    const std::vector<NumberMention> written = numbers("3.14");
    ASSERT_EQ(spoken.size(), 1U);
    ASSERT_EQ(written.size(), 1U);
    EXPECT_EQ(spoken[0].value, written[0].value) << "equal to the last bit, not merely close";
}

TEST(Numbers, YearsSaidInPairsAreRead) {
    EXPECT_EQ(read("nineteen eighty-four, twenty twenty, nineteen oh five, fifteen ninety"),
              (All{{1984, "nineteen eighty-four"},
                   {2020, "twenty twenty"},
                   {1905, "nineteen oh five"},
                   {1590, "fifteen ninety"}}));
}

TEST(Numbers, PairsOutsideTheCenturiesOfYearsAreTwoNumbers) {
    EXPECT_EQ(read("at ten thirty"), (All{{10, "ten"}, {30, "thirty"}})) << "a time";
    EXPECT_EQ(read("twenty one"), (All{{21, "twenty one"}}));
    EXPECT_EQ(read("nineteen hundred"), (All{{1900, "nineteen hundred"}}));
}

TEST(Numbers, SignsMakeNegatives) {
    EXPECT_EQ(read("-5, minus five, negative five, \u22125, (-3)"),
              (All{{-5, "-5"}, {-5, "minus five"}, {-5, "negative five"}, {-5, "\u22125"}, {-3, "-3"}}));
}

TEST(Numbers, AHyphenAfterAWordOrNumberIsNoSign) {
    EXPECT_EQ(read("COVID-19 and 10-20"), (All{{19, "19"}, {10, "10"}, {20, "20"}}));
    EXPECT_EQ(read("5%-10%"), (All{percent(5, "5%"), percent(10, "10%")}));
}

TEST(Numbers, PercentsAreMarked) {
    EXPECT_EQ(read("37%, 12 %, thirty-seven percent, 5 per cent, 3\uFF05"),
              (All{percent(37, "37%"), percent(12, "12 %"), percent(37, "thirty-seven percent"),
                   percent(5, "5 per cent"), percent(3, "3\uFF05")}));
}

TEST(Numbers, OrdinalsAreMarked) {
    EXPECT_EQ(
        read("the 21st, 2nd and 3rd; the third, twenty-first, one hundred third and hundredth time"),
        (All{ordinal(21, "21st"), ordinal(2, "2nd"), ordinal(3, "3rd"), ordinal(3, "third"),
             ordinal(21, "twenty-first"), ordinal(103, "one hundred third"), ordinal(100, "hundredth")}));
}

TEST(Numbers, AnOrdinalIsNoPercentage) {
    EXPECT_EQ(read("the 1st %"), (All{ordinal(1, "1st")}));
}

TEST(Numbers, AHyphenJoinsOnlyTheWordsItTouches) {
    EXPECT_EQ(read("thirty - seven"), (All{{30, "thirty"}, {7, "seven"}})) << "a dash between two numbers";
}

TEST(Numbers, ADecadeIsItsFirstYear) {
    EXPECT_EQ(read("the 1990s"), (All{{1990, "1990s"}}));
}

TEST(Numbers, ARangeSharesItsUnit) {
    EXPECT_EQ(read("5-10%, 5 to 10 percent"),
              (All{percent(5, "5"), percent(10, "10%"), percent(5, "5"), percent(10, "10 percent")}));
    EXPECT_EQ(read("between 5 and 10 million, 5 or 6 billion"),
              (All{{5e6, "5"}, {10e6, "10 million"}, {5e9, "5"}, {6e9, "6 billion"}}));
}

TEST(Numbers, AScaleIsSharedOnlyByAComparableNumber) {
    EXPECT_EQ(read("from 500 to 1 million"), (All{{500, "500"}, {1e6, "1 million"}}));
    EXPECT_EQ(read("from 20 to 10 million"), (All{{20e6, "20"}, {10e6, "10 million"}})) << "a fall";
}

TEST(Numbers, NumbersInARowStayApart) {
    EXPECT_EQ(read("five six, one hundred and two hundred, five thousand three million"),
              (All{{5, "five"},
                   {6, "six"},
                   {100, "one hundred"},
                   {200, "two hundred"},
                   {5000, "five thousand"},
                   {3e6, "three million"}}));
    EXPECT_EQ(read("1,2,3"), (All{{1, "1"}, {2, "2"}, {3, "3"}}));
    EXPECT_EQ(read("a thousand thousand"), (All{{1000, "a thousand"}})) << "a scale after a scale is bare";
}

TEST(Numbers, FractionsArePassedOver) {
    EXPECT_TRUE(
        read("one third, two thirds, three quarters, one half, a third of them, a thousandth of a second")
            .empty());
    EXPECT_EQ(read("a third time"), (All{ordinal(3, "third")})) << "an ordinal, not a fraction";
}

TEST(Numbers, OneAsAPronounIsNoNumber) {
    EXPECT_TRUE(read("no one came; the one; each one; one another").empty());
    EXPECT_EQ(read("one of them"), (All{{1, "one"}}));
}

TEST(Numbers, SecondAsTimeIsNoNumber) {
    EXPECT_TRUE(read("a second; per second; a split second; seconds").empty());
    EXPECT_EQ(read("the second time"), (All{ordinal(2, "second")}));
}

TEST(Numbers, WhatIsNoQuantityIsNotRead) {
    EXPECT_TRUE(read("3D GPT4 5k half twice dozens hundreds the hundred a few").empty());
    EXPECT_TRUE(read("at this point").empty());
    EXPECT_TRUE(read("minus").empty());
}

TEST(Numbers, AValueTooLargeIsNotRead) {
    EXPECT_TRUE(read(std::string(400, '9')).empty()) << "beyond a double";
    EXPECT_TRUE(read(std::string(300, '9') + " trillion").empty())
        << "a quantity a double cannot hold is not read, rather than read as a smaller one";
}

TEST(Numbers, ValuesAreComparedWithinARoundingError) {
    EXPECT_TRUE(same_value(0.1 + 0.2, 0.3));
    EXPECT_TRUE(same_value(1e6, 1e6 + 1e-4));
    EXPECT_FALSE(same_value(1e6, 1e6 + 1.0));
    EXPECT_FALSE(same_value(37.0, 37.5));
    EXPECT_TRUE(same_value(0.0, -0.0));
}

TEST(Numbers, ANumberIsItsValueAndItsUnit) {
    const std::vector<NumberMention> found = numbers("37%, thirty-seven percent, 37, 37th");
    ASSERT_EQ(found.size(), 4U);
    EXPECT_TRUE(same_number(found[0], found[1]));
    EXPECT_FALSE(same_number(found[0], found[2])) << "a percentage is not a count";
    EXPECT_FALSE(same_number(found[2], found[3])) << "an ordinal is not a count";
}

}  // namespace
}  // namespace rr::text

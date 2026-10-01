#include <rr/text/align.hpp>

#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace rr::text {
namespace {

constexpr std::string_view kSpeech = "We choose to go to the Moon in this decade and do the other things.";

TEST(Align, AVerbatimQuoteIsFound) {
    const Alignment found = align("we choose to go to the moon", kSpeech);
    EXPECT_TRUE(found.verbatim());
    EXPECT_EQ(found.quote_terms, 7U);
    EXPECT_EQ(found.matched, 7U);
    EXPECT_EQ(found.edits, 0U);
    EXPECT_DOUBLE_EQ(found.identity(), 1.0);
    EXPECT_EQ(found.score, 14);
    EXPECT_EQ(text_of(kSpeech, found.text), "We choose to go to the Moon");
}

TEST(Align, PunctuationCaseAndAccentsAreNotCompared) {
    EXPECT_TRUE(align("\"We choose... to go to the MOON!\"", kSpeech).verbatim());
    EXPECT_TRUE(align("caf\u00E9 au lait", "a CAFE AU LAIT, please").verbatim());
}

TEST(Align, ASoftenedQuoteIsNotVerbatim) {
    constexpr std::string_view kText = "Honestly, the plan is terrible and we know it.";
    const Alignment found = align("the plan is bad and we know it", kText);
    EXPECT_FALSE(found.verbatim());
    EXPECT_EQ(found.matched, 7U);
    EXPECT_EQ(found.edits, 1U) << "one word differs";
    EXPECT_NEAR(found.identity(), 7.0 / 8.0, 1e-12);
    EXPECT_EQ(text_of(kText, found.text), "the plan is terrible and we know it")
        << "the transcript's own words, for the repair message";
}

TEST(Align, AWordTheTextAddsIsAGap) {
    const Alignment found = align("I think it is bad", "I think it is really bad.");
    EXPECT_FALSE(found.verbatim());
    EXPECT_EQ(found.matched, 5U);
    EXPECT_EQ(found.edits, 1U);
    EXPECT_DOUBLE_EQ(found.identity(), 1.0) << "every quote word is there, just not in a row";
}

TEST(Align, AWordTheQuoteAddsIsAGap) {
    const Alignment found = align("we choose to go now to the moon", kSpeech);
    EXPECT_FALSE(found.verbatim());
    EXPECT_EQ(found.matched, 7U);
    EXPECT_EQ(found.edits, 1U);
    EXPECT_NEAR(found.identity(), 7.0 / 8.0, 1e-12);
}

TEST(Align, OnlyTheAlignedStretchIsReported) {
    constexpr std::string_view kQuote = "Frankly, we choose to go to the moon";
    const Alignment found = align(kQuote, kSpeech);
    EXPECT_FALSE(found.verbatim()) << "\"Frankly\" was never said";
    EXPECT_EQ(found.matched, 7U);
    EXPECT_EQ(found.edits, 0U) << "a stretch is aligned, not the whole quote";
    EXPECT_EQ(text_of(kQuote, found.quote), "we choose to go to the moon");
}

TEST(Align, TheFirstOfEqualPlacesIsChosen) {
    constexpr std::string_view kText = "one two three. four. one two three.";
    const Alignment found = align("one two three", kText);
    EXPECT_TRUE(found.verbatim());
    EXPECT_EQ(found.text, (Span{.begin = 0, .end = 13}));
}

TEST(Align, NothingInCommonAlignsNothing) {
    const Alignment found = align("completely different words", kSpeech);
    EXPECT_EQ(found.score, 0);
    EXPECT_EQ(found.matched, 0U);
    EXPECT_EQ(found.quote_terms, 3U);
    EXPECT_EQ(found.text, Span{});
    EXPECT_FALSE(found.verbatim());
    EXPECT_DOUBLE_EQ(found.identity(), 0.0);
}

TEST(Align, AQuoteWithoutWordsIsNeverVerbatim) {
    const Alignment found = align("?!", kSpeech);
    EXPECT_EQ(found.quote_terms, 0U);
    EXPECT_FALSE(found.verbatim());
    EXPECT_DOUBLE_EQ(found.identity(), 0.0);
    EXPECT_FALSE(align("words", "").verbatim());
}

TEST(Align, ScoresSetWhatAGapIsWorth) {
    constexpr std::string_view kQuote = "alpha beta gamma delta";
    constexpr std::string_view kText = "alpha beta x y gamma delta";
    const Alignment cheap = align(kQuote, kText, {.match = 2, .mismatch = -1, .gap = -1});
    EXPECT_EQ(cheap.matched, 4U) << "two gaps cost less than two matches gain";
    const Alignment dear = align(kQuote, kText, {.match = 2, .mismatch = -5, .gap = -5});
    EXPECT_EQ(dear.matched, 2U) << "bridging costs more: one side is aligned alone";
    EXPECT_EQ(dear.edits, 0U);
}

TEST(Align, ALocalAlignmentRestartsRatherThanCarryALoss) {
    constexpr std::string_view kText = "a x b c";
    const Alignment found = align("a b c", kText, {.match = 2, .mismatch = -3, .gap = -3});
    EXPECT_EQ(found.score, 4) << R"("b c" alone: bridging "a" would cost more than it gains)";
    EXPECT_EQ(found.matched, 2U);
    EXPECT_EQ(text_of(kText, found.text), "b c");
}

TEST(Align, TermsComputedOnceGiveTheSameAlignment) {
    const Alignment direct = align("to go to the moon", kSpeech);
    EXPECT_EQ(align(terms("to go to the moon"), terms(kSpeech)), direct);
}

TEST(AlignDeathTest, ScoresThatCannotAlignAreRefused) {
    EXPECT_DEATH((void)align("a", "a", {.match = 0}), "a match must gain");
    EXPECT_DEATH((void)align("a", "a", {.gap = 0}), "a gap must cost");
}

}  // namespace
}  // namespace rr::text

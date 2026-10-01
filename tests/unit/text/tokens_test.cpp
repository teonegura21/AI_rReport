#include <rr/text/tokens.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace rr::text {
namespace {

// Each token as its kind and its text.
std::vector<std::pair<TokenKind, std::string>> seen(std::string_view text) {
    std::vector<std::pair<TokenKind, std::string>> found;
    for (const Token& token : tokenize(text)) {
        found.emplace_back(token.kind, std::string{text_of(text, token.span)});
    }
    return found;
}

std::vector<std::string> texts(std::string_view text) {
    std::vector<std::string> found;
    for (const Token& token : tokenize(text)) {
        found.emplace_back(text_of(text, token.span));
    }
    return found;
}

constexpr TokenKind kWord = TokenKind::word;
constexpr TokenKind kNumber = TokenKind::number;
constexpr TokenKind kPunctuation = TokenKind::punctuation;
constexpr TokenKind kSymbol = TokenKind::symbol;

TEST(Tokens, SplitWordsNumbersPunctuationAndSymbols) {
    using Seen = std::vector<std::pair<TokenKind, std::string>>;
    EXPECT_EQ(seen("Hello, world! It costs $42."), (Seen{{kWord, "Hello"},
                                                         {kPunctuation, ","},
                                                         {kWord, "world"},
                                                         {kPunctuation, "!"},
                                                         {kWord, "It"},
                                                         {kWord, "costs"},
                                                         {kSymbol, "$"},
                                                         {kNumber, "42"},
                                                         {kPunctuation, "."}}));
}

TEST(Tokens, KnowWhereTheyAre) {
    const std::vector<Token> tokens = tokenize("ab  \u00E9t\u00E9!");
    ASSERT_EQ(tokens.size(), 3U);
    EXPECT_EQ(tokens[0].span, (Span{.begin = 0, .end = 2}));
    EXPECT_EQ(tokens[1].span, (Span{.begin = 4, .end = 9})) << "byte offsets: \u00E9 is two bytes";
    EXPECT_EQ(tokens[2].span, (Span{.begin = 9, .end = 10}));
}

TEST(Tokens, AWordHoldsLettersMarksAndDigits) {
    EXPECT_EQ(seen("GPT4 3D 1st"), (std::vector<std::pair<TokenKind, std::string>>{
                                       {kWord, "GPT4"}, {kWord, "3D"}, {kWord, "1st"}}));
    EXPECT_EQ(texts("e\u0301te\u0301"), std::vector<std::string>{"e\u0301te\u0301"})
        << "combining accents stay with their letters";
    EXPECT_EQ(texts("\u4E2D\u6587 \u0928\u092E\u0938\u094D\u0924\u0947"),
              (std::vector<std::string>{"\u4E2D\u6587", "\u0928\u092E\u0938\u094D\u0924\u0947"}));
}

TEST(Tokens, AnApostropheBetweenLettersStaysInTheWord) {
    EXPECT_EQ(texts("don't l\u2019\u00E9t\u00E9"),
              (std::vector<std::string>{"don't", "l\u2019\u00E9t\u00E9"}));
    EXPECT_EQ(texts("dogs' 'quoted'"), (std::vector<std::string>{"dogs", "'", "'", "quoted", "'"}));
    EXPECT_EQ(texts("90's"), (std::vector<std::string>{"90", "'", "s"})) << "between digits, it splits";
}

TEST(Tokens, AHyphenSplits) {
    EXPECT_EQ(texts("state-of-the-art"),
              (std::vector<std::string>{"state", "-", "of", "-", "the", "-", "art"}));
    EXPECT_EQ(texts("thirty-seven"), (std::vector<std::string>{"thirty", "-", "seven"}));
}

TEST(Tokens, APointBetweenDigitsIsADecimalPoint) {
    EXPECT_EQ(seen("3.5 v1.2 3.5.1"),
              (std::vector<std::pair<TokenKind, std::string>>{
                  {kNumber, "3.5"}, {kWord, "v1"}, {kPunctuation, "."}, {kNumber, "2"}, {kNumber, "3.5.1"}}))
        << "a word that started with a letter ends at the point";
    EXPECT_EQ(texts("It is 3. Then"), (std::vector<std::string>{"It", "is", "3", ".", "Then"}));
}

TEST(Tokens, ACommaBeforeExactlyThreeDigitsSeparatesThousands) {
    EXPECT_EQ(texts("1,234,567 and 12,345.67"), (std::vector<std::string>{"1,234,567", "and", "12,345.67"}));
    EXPECT_EQ(texts("1,2,3"), (std::vector<std::string>{"1", ",", "2", ",", "3"}));
    EXPECT_EQ(texts("1,2345"), (std::vector<std::string>{"1", ",", "2345"})) << "four digits: a list";
    EXPECT_EQ(texts("1,23 and"), (std::vector<std::string>{"1", ",", "23", "and"})) << "two digits: a list";
    EXPECT_EQ(texts("1,234"), (std::vector<std::string>{"1,234"})) << "three digits at the end";
    EXPECT_EQ(texts("1,"), (std::vector<std::string>{"1", ","}));
}

TEST(Tokens, SpacesAndControlsSeparateAndAreNoTokens) {
    EXPECT_EQ(texts("a\tb\u00A0c\u200Bd\ne"), (std::vector<std::string>{"a", "b", "c", "d", "e"}));
    EXPECT_TRUE(tokenize("").empty());
    EXPECT_TRUE(tokenize(" \t\n").empty());
}

TEST(Tokens, AnInvalidByteIsASymbol) {
    EXPECT_EQ(
        seen("a\x80"
             "b"),
        (std::vector<std::pair<TokenKind, std::string>>{{kWord, "a"}, {kSymbol, "\x80"}, {kWord, "b"}}));
}

TEST(Tokens, AWordOfMarksAloneIsNoTerm) {
    constexpr std::string_view kText = "्́ word";  // found by fuzzing
    ASSERT_EQ(tokenize(kText).size(), 2U);
    EXPECT_EQ(tokenize(kText)[0].kind, TokenKind::word) << "a mark starts a word";
    const std::vector<Term> found = terms(kText);
    ASSERT_EQ(found.size(), 1U) << "but folding removes marks: nothing is left to compare";
    EXPECT_EQ(found[0].folded, "word");
}

TEST(Tokens, TermsAreTheFoldedWordsAndNumbers) {
    const std::vector<Term> found = terms("The Caf\u00E9\u2019s \u2014 3.5%");
    ASSERT_EQ(found.size(), 3U);
    EXPECT_EQ(found[0].folded, "the");
    EXPECT_EQ(found[1].folded, "cafe's");
    EXPECT_EQ(found[1].span, (Span{.begin = 4, .end = 13}));
    EXPECT_EQ(found[2].folded, "3.5");
}

}  // namespace
}  // namespace rr::text

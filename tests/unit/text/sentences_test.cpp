#include <rr/text/sentences.hpp>

#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "covered.hpp"

namespace rr::text {
namespace {

using Texts = std::vector<std::string>;

Texts split(std::string_view text) {
    return covered(text, sentences(text));
}

TEST(Sentences, EndAfterATerminatorAndASpace) {
    EXPECT_EQ(split("It rained. We stayed in! Did you? Yes."),
              (Texts{"It rained.", "We stayed in!", "Did you?", "Yes."}));
    EXPECT_EQ(split("No terminator at the end"), (Texts{"No terminator at the end"}));
}

TEST(Sentences, KnowWhereTheyAre) {
    const std::vector<Span> found = sentences("  One.  Two.  ");
    EXPECT_EQ(found, (std::vector<Span>{{.begin = 2, .end = 6}, {.begin = 8, .end = 12}})) << "trimmed";
}

TEST(Sentences, ATerminatorInsideAWordOrNumberEndsNothing) {
    EXPECT_EQ(split("Version 3.5 is out. See example.com now."),
              (Texts{"Version 3.5 is out.", "See example.com now."}));
    EXPECT_EQ(split("Wait!Really"), (Texts{"Wait!Really"}));
}

TEST(Sentences, ClosingQuotesAndBracketsBelongToTheSentence) {
    EXPECT_EQ(split("He said \"Stop.\" Then he left."), (Texts{"He said \"Stop.\"", "Then he left."}));
    EXPECT_EQ(split("(See above.) Next one."), (Texts{"(See above.)", "Next one."}));
    EXPECT_EQ(split("\u00ABFini.\u00BB Puis rien."), (Texts{"\u00ABFini.\u00BB", "Puis rien."}));
    EXPECT_EQ(split("She wrote \u201Cyes.\u201D He agreed."),
              (Texts{"She wrote \u201Cyes.\u201D", "He agreed."}));
}

TEST(Sentences, ALowercaseWordContinuesAfterAQuestionOrExclamation) {
    EXPECT_EQ(split("\"Why?\" she asked. \"Because!\" he said."),
              (Texts{"\"Why?\" she asked.", "\"Because!\" he said."}));
    EXPECT_EQ(split("Really?! It worked."), (Texts{"Really?!", "It worked."}));
}

TEST(Sentences, AnEllipsisEndsOneOnlyBeforeACapital) {
    EXPECT_EQ(split("And then... we left. Later\u2026 Nobody knew."),
              (Texts{"And then... we left.", "Later\u2026", "Nobody knew."}));
    EXPECT_EQ(split("It went 10... 20... 30."), (Texts{"It went 10... 20... 30."}));
    EXPECT_EQ(split("He paused\u2026 \u201CNever,\u201D he said."),
              (Texts{"He paused\u2026", "\u201CNever,\u201D he said."}))
        << "the capital is looked for past an opening quote";
}

TEST(Sentences, AnAbbreviationThatNeverEndsOneDoesNot) {
    EXPECT_EQ(split("Dr. Smith met Mr. Jones. They talked."),
              (Texts{"Dr. Smith met Mr. Jones.", "They talked."}));
    EXPECT_EQ(split("Fruit, e.g. Apples, is good. Roe v. Wade was decided."),
              (Texts{"Fruit, e.g. Apples, is good.", "Roe v. Wade was decided."}));
}

TEST(Sentences, AnAbbreviationThatOftenEndsOneDoesBeforeAFunctionWord) {
    EXPECT_EQ(split("He served in the U.S. Army. Apple Inc. It grew. Oil etc. and gas."),
              (Texts{"He served in the U.S. Army.", "Apple Inc.", "It grew.", "Oil etc. and gas."}));
    EXPECT_EQ(split("We met at 3 p.m. Then we left."), (Texts{"We met at 3 p.m.", "Then we left."}));
}

TEST(Sentences, ANumberingAbbreviationDoesNotEndOneBeforeANumber) {
    EXPECT_EQ(split("Turn to p. 5 now. See Fig. 3 below. That is a no. Next."),
              (Texts{"Turn to p. 5 now.", "See Fig. 3 below.", "That is a no.", "Next."}));
}

TEST(Sentences, AnInitialDoesNotEndOneBeforeAName) {
    EXPECT_EQ(split("John F. Kennedy spoke. J. K. Rowling wrote. We need a plan B. Then we go."),
              (Texts{"John F. Kennedy spoke.", "J. K. Rowling wrote.", "We need a plan B.", "Then we go."}));
}

TEST(Sentences, APeriodBeforeALowercaseWordEndsNothing) {
    EXPECT_EQ(split("Buy a 2 lb. bag of rice. Cook it."), (Texts{"Buy a 2 lb. bag of rice.", "Cook it."}));
}

TEST(Sentences, IdeographicTerminatorsNeedNoSpace) {
    EXPECT_EQ(split("\u6211\u5F88\u597D\u3002\u4F60\u5462\uFF1F\u597D\uFF01"),
              (Texts{"\u6211\u5F88\u597D\u3002", "\u4F60\u5462\uFF1F", "\u597D\uFF01"}));
}

TEST(Sentences, OtherScriptsTerminatorsEndThem) {
    EXPECT_EQ(split("\u0645\u0631\u062D\u0628\u0627\u061F \u0646\u0639\u0645."),
              (Texts{"\u0645\u0631\u062D\u0628\u0627\u061F", "\u0646\u0639\u0645."}))
        << "the Arabic question mark";
    EXPECT_EQ(split("\u0928\u092E\u0938\u094D\u0924\u0947\u0964 \u0920\u0940\u0915\u0964"),
              (Texts{"\u0928\u092E\u0938\u094D\u0924\u0947\u0964", "\u0920\u0940\u0915\u0964"}))
        << "the danda";
}

TEST(Sentences, AParagraphBreakAlwaysEndsOne) {
    EXPECT_EQ(split("A heading\n\nThe text starts here"), (Texts{"A heading", "The text starts here"}));
    EXPECT_EQ(split("A line\r\n \r\nAnother"), (Texts{"A line", "Another"})) << "blank, with spaces in it";
    EXPECT_EQ(split("One\u2029Two"), (Texts{"One", "Two"})) << "the paragraph separator";
    EXPECT_EQ(split("A caption line\nwraps here."), (Texts{"A caption line\nwraps here."}))
        << "a single line break is a space";
    EXPECT_EQ(split("A line\r\nwraps"), (Texts{"A line\r\nwraps"})) << "CRLF is one break";
}

TEST(Sentences, AFragmentWithoutAWordJoinsTheSentenceBefore) {
    EXPECT_EQ(split("Hello. . . World."), (Texts{"Hello. . .", "World."}));
    EXPECT_EQ(split("Music plays. \u266A\u266A. Then words."),
              (Texts{"Music plays. \u266A\u266A.", "Then words."}));
}

TEST(Sentences, AFragmentAtTheStartJoinsTheSentenceAfter) {
    EXPECT_EQ(split("\u266A! Then words. More."), (Texts{"\u266A! Then words.", "More."}));
}

TEST(Sentences, NothingButFragmentsIsOneSentence) {
    EXPECT_EQ(split("\u266A\u266A"), (Texts{"\u266A\u266A"}));
    EXPECT_EQ(split("?! ..."), (Texts{"?! ..."}));
}

TEST(Sentences, ATextOfSpacesHasNone) {
    EXPECT_TRUE(sentences("").empty());
    EXPECT_TRUE(sentences(" \n\t\u00A0").empty());
}

TEST(Sentences, InvalidBytesAreKept) {
    EXPECT_EQ(split("Bad \xFF byte. Fine."), (Texts{"Bad \xFF byte.", "Fine."}));
}

}  // namespace
}  // namespace rr::text

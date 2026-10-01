#include <rr/text/entities.hpp>

#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "covered.hpp"

namespace rr::text {
namespace {

using Texts = std::vector<std::string>;

Texts names_in(std::string_view text) {
    return covered(text, names(text));
}

Texts mentions_in(std::string_view text, std::string_view term) {
    return covered(text, mentions(text, term));
}

TEST(Names, AreRunsOfCapitalizedWords) {
    EXPECT_EQ(names_in("Marie Curie won the Nobel Prize in Paris with Pierre Curie."),
              (Texts{"Marie Curie", "Nobel Prize", "Paris", "Pierre Curie"}));
}

TEST(Names, AcronymsAndInnerCapitalsAreNames) {
    EXPECT_EQ(names_in("Then NASA bought an iPhone from McDonald."), (Texts{"NASA", "iPhone", "McDonald"}));
}

TEST(Names, ConnectorsJoinCapitalizedWords) {
    EXPECT_EQ(names_in("We met the Bank of America, Leonardo da Vinci and Pirates of the Caribbean fans."),
              (Texts{"Bank of America", "Leonardo da Vinci", "Pirates of the Caribbean"}));
    EXPECT_EQ(names_in("He drove Paris of the old days."), (Texts{"Paris"}))
        << "a connector joins only capitalized words";
}

TEST(Names, AmpersandsAndJoiningHyphensJoin) {
    EXPECT_EQ(names_in("So AT&T, Johnson & Johnson and Jean-Paul Sartre studied COVID-19."),
              (Texts{"AT&T", "Johnson & Johnson", "Jean-Paul Sartre", "COVID-19"}));
}

TEST(Names, AHyphenJoinsOnlyWordsItTouches) {
    EXPECT_EQ(names_in("Then Paris - Berlin and Rome -Milan trains."),
              (Texts{"Paris", "Berlin", "Rome", "Milan"}));
}

TEST(Names, ANumberMayFollowACapitalizedWord) {
    EXPECT_EQ(names_in("Then Apollo 11 landed, and the iPhone 15 sold."), (Texts{"Apollo 11", "iPhone 15"}));
    EXPECT_EQ(names_in("Then Apollo 11 20 times."), (Texts{"Apollo 11"})) << "one number, after a word";
}

TEST(Names, TitlesAndInitialsKeepTheirPeriod) {
    EXPECT_EQ(names_in("Then Dr. Smith met John F. Kennedy and J. K. Rowling in the U.S. Army."),
              (Texts{"Dr. Smith", "John F. Kennedy", "J. K. Rowling", "U.S. Army"}));
}

TEST(Names, FunctionWordsAtTheHeadAreLeftOut) {
    EXPECT_EQ(names_in("The Beatles played. Yesterday Apple did. In 2019 Google did."),
              (Texts{"Beatles", "Apple", "Google"}));
}

TEST(Names, AFunctionWordInCapitalsIsAnAcronym) {
    EXPECT_EQ(names_in("Then US officials and IT staff came."), (Texts{"US", "IT"}));
}

TEST(Names, ASingleCapitalizedWordThatStartsASentenceOrQuoteIsNone) {
    EXPECT_TRUE(names_in("Paris is lovely.").empty());
    EXPECT_EQ(names_in("\"Hello,\" said Tom."), (Texts{"Tom"}));
    EXPECT_TRUE(names_in("He said \"Hello\" twice, and her motto: Persevere.").empty())
        << "a quote or a colon starts a clause inside a sentence";
    EXPECT_EQ(names_in("NASA agreed. iPhones sold."), (Texts{"NASA", "iPhones"}));
}

TEST(Names, IIsNoName) {
    EXPECT_EQ(names_in("Then I met Anna, and I'm sure I've seen Bob."), (Texts{"Anna", "Bob"}));
    EXPECT_EQ(names_in("Yesterday Tom I think left."), (Texts{"Tom"})) << "\"I\" ends a name before it";
}

TEST(Names, APossessiveEndsAName) {
    EXPECT_EQ(names_in("We read Marie Curie's notes and Sartre\u2019s COVID-19 study."),
              (Texts{"Marie Curie", "Sartre", "COVID-19"}));
}

TEST(Names, CaseSaysNothingInASentenceInCapitals) {
    EXPECT_TRUE(names_in("THIS IS ALL CAPITALS.").empty());
    EXPECT_EQ(names_in("THIS IS LOUD. Then Anna spoke."), (Texts{"Anna"}));
}

TEST(Names, ATextWithoutCapitalsHasNone) {
    EXPECT_TRUE(names_in("auto captions are all lowercase like marie curie").empty());
    EXPECT_TRUE(names("").empty());
}

TEST(Mentions, AreTheTermsWordsInARow) {
    EXPECT_EQ(mentions_in("Marie Curie met marie curie, not Marie Sk\u0142odowska Curie.", "Marie Curie"),
              (Texts{"Marie Curie", "marie curie"}));
}

TEST(Mentions, AreComparedFolded) {
    EXPECT_EQ(mentions_in("The CAF\u00C9 and the cafe", "caf\u00E9"), (Texts{"CAF\u00C9", "cafe"}));
    EXPECT_EQ(mentions_in("It is state of the art.", "state-of-the-art"), (Texts{"state of the art"}));
}

TEST(Mentions, APossessiveMentionsTheTerm) {
    EXPECT_EQ(mentions_in("Curie's work and Curie\u2019s notes", "Curie"),
              (Texts{"Curie's", "Curie\u2019s"}));
    EXPECT_TRUE(mentions_in("Curies", "Curie").empty()) << "another word";
}

TEST(Mentions, DoNotOverlap) {
    EXPECT_EQ(mentions_in("ha ha ha", "ha ha"), (Texts{"ha ha"}));
}

TEST(Mentions, ATermWithoutWordsIsMentionedNowhere) {
    EXPECT_TRUE(mentions("anything", "").empty());
    EXPECT_TRUE(mentions("anything", "!?").empty());
}

}  // namespace
}  // namespace rr::text

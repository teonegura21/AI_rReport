#include <rr/transcript/projection.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>

namespace rr::transcript {
namespace {

using testkit::unwrap;

// Automatic captions: `text`'s words, a second each.
std::vector<Word> captions(std::string_view text) {
    std::vector<Word> words;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t end = std::min(text.find(' ', at), text.size());
        const auto second = static_cast<std::int64_t>(words.size()) * 1000;
        words.push_back(Word{.text = std::string{text.substr(at, end - at)},
                             .start = Millis{second},
                             .end = Millis{second + 900}});
        at = end + 1;
    }
    return words;
}

std::string joined(const Projection& projection) {
    std::string text;
    for (const Word& word : projection.words) {
        text += text.empty() ? word.text : " " + word.text;
    }
    return text;
}

TEST(Projection, LaysTheModelsPunctuationAndCapitalsOnTheWords) {
    const std::vector<Word> said =
        captions("so marie curie won two nobel prizes she called radium the most beautiful element");
    const Projection projection = unwrap(project(
        said,
        "So Marie Curie won two Nobel Prizes. She called radium \u201Cthe most beautiful element.\u201D"));
    EXPECT_EQ(
        joined(projection),
        "So Marie Curie won two Nobel Prizes. She called radium \u201Cthe most beautiful element.\u201D");
    EXPECT_EQ(projection.matched, 14U);
    ASSERT_EQ(projection.words.size(), said.size());
    for (std::size_t index = 0; index < said.size(); ++index) {
        EXPECT_EQ(projection.words[index].start, said[index].start) << "each word keeps its time";
    }
}

TEST(Projection, AWordTheModelChangedIsNotTaken) {
    const Projection projection = unwrap(project(
        captions("revenue grew thirty seven percent in twenty twenty three"), "Revenue grew 37% in 2023."));
    EXPECT_EQ(joined(projection), "Revenue grew thirty seven percent in twenty twenty three.");
    EXPECT_EQ(projection.matched, 3U);
    EXPECT_EQ(projection.changed, 2U);
    EXPECT_EQ(projection.dropped, 4U);
}

TEST(Projection, NeitherAHyphenNorASymbolIsAdded) {
    EXPECT_EQ(joined(unwrap(project(captions("a well known brand grew 5 points"),
                                    "A well-known brand \u00A7 grew 5% points."))),
              "A well known brand grew 5 points.")
        << "a hyphen joins words; a percent changes a number";
}

TEST(Projection, AnAddedWordIsNotTakenButItsSentenceEndIs) {
    const Projection projection =
        unwrap(project(captions("we went home so that was it"), "We went home, um. So that was it."));
    EXPECT_EQ(joined(projection), "We went home. So that was it.");
    EXPECT_EQ(projection.added, 1U);
    EXPECT_EQ(joined(unwrap(project(captions("is it home so"), "Is it home? Um. So."))), "Is it home? So.")
        << "a word that ends a sentence already keeps its end";
}

TEST(Projection, ADroppedWordStaysAsSaid) {
    const Projection projection = unwrap(project(captions("i i think so"), "I think so."));
    EXPECT_EQ(joined(projection), "i I think so.");
    EXPECT_EQ(projection.dropped, 1U);
}

TEST(Projection, TheCaptionsOwnPunctuationStays) {
    EXPECT_EQ(joined(unwrap(project(captions("in the u.s. today"), "In the U.S., today."))),
              "In the U.S. today.");
    EXPECT_EQ(joined(unwrap(project(captions("the (big) one"), "The \u201Cbig\u201D one."))),
              "The (big) one.");
}

TEST(Projection, QuotesAndBracketsGoWhereTheModelPutThem) {
    EXPECT_EQ(joined(unwrap(project(captions("she said that was it and left quietly"),
                                    "She said, \u201CThat was it,\u201D and left (quietly)."))),
              "She said, \u201CThat was it,\u201D and left (quietly).");
    EXPECT_EQ(joined(unwrap(project(captions("she left quietly"), "She left(quietly)."))),
              "She left (quietly).")
        << "an opening bracket belongs to the word after it, space or not";
}

TEST(Projection, OnlyCapitalsChange) {
    EXPECT_EQ(joined(unwrap(project(captions("my resume"), "My R\u00E9sum\u00E9."))), "My resume.")
        << "accents are not capitals";
}

TEST(Projection, AModelWithoutWordsLeavesTheCaptions) {
    const Projection projection = unwrap(project(captions("so it goes"), "..."));
    EXPECT_EQ(joined(projection), "so it goes");
    EXPECT_EQ(projection.dropped, 3U);
}

TEST(Projection, TooLongToAlignAtOnceIsRefused) {
    std::string many;
    for (std::size_t word = 0; word <= kMaxProjected; ++word) {
        many += many.empty() ? "word" : " word";
    }
    const Result<Projection> projection = project(captions(many), "Word.");
    ASSERT_FALSE(projection.has_value());
    EXPECT_EQ(projection.error().code(), Errc::invalid_argument);
}

}  // namespace
}  // namespace rr::transcript

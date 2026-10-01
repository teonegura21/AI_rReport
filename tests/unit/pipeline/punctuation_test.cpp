#include <rr/pipeline/punctuation.hpp>

#include <chrono>
#include <cstddef>
#include <format>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>

namespace rr::pipeline {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using testkit::unwrap;
using transcript::Word;

// `count` words, "w0" to "w<count-1>", said one every 300 ms without a pause.
std::vector<Word> words(std::size_t count) {
    std::vector<Word> said;
    for (std::size_t index = 0; index < count; ++index) {
        const Millis start{static_cast<std::int64_t>(index) * 300};
        said.push_back(Word{.text = std::format("w{}", index), .start = start, .end = start + Millis{300}});
    }
    return said;
}

// The words of `text`, said one every 300 ms.
std::vector<Word> words(std::string_view text) {
    std::vector<Word> said;
    std::size_t begin = 0;
    while (begin < text.size()) {
        const std::size_t end = std::min(text.find(' ', begin), text.size());
        const Millis start{static_cast<std::int64_t>(said.size()) * 300};
        said.push_back(Word{.text = std::string{text.substr(begin, end - begin)},
                            .start = start,
                            .end = start + Millis{300}});
        begin = end + 1;
    }
    return said;
}

// Moves every word from `index` on by `pause`: the speaker paused before it.
void pause_before(std::vector<Word>& said, std::size_t index, Millis pause) {
    for (std::size_t later = index; later < said.size(); ++later) {
        said[later].start += pause;
        said[later].end += pause;
    }
}

// The bytes of words [begin, end) joined by spaces.
std::size_t bytes(const std::vector<Word>& said, std::size_t begin, std::size_t end) {
    std::size_t total = 0;
    for (std::size_t index = begin; index < end; ++index) {
        total += said[index].text.size() + (index > begin ? 1 : 0);
    }
    return total;
}

constexpr WindowSize kSmall{.least = 60, .target = 100, .most = 140, .context = 20};

TEST(Windows, EveryWordIsInExactlyOneWindowAndEachHasItsSize) {
    const std::vector<Word> said = words(600);
    const std::vector<Window> windows = windows_of(said, kSmall);
    ASSERT_GT(windows.size(), 10U);
    std::size_t next = 0;
    for (const Window& window : windows) {
        EXPECT_EQ(window.begin, next) << "in order, with nothing between them";
        EXPECT_LT(window.begin, window.end);
        EXPECT_LE(bytes(said, window.begin, window.end), kSmall.most);
        if (&window != &windows.back()) {
            EXPECT_GE(bytes(said, window.begin, window.end), kSmall.least);
        }
        next = window.end;
    }
    EXPECT_EQ(next, said.size()) << "every word is in one";
}

TEST(Windows, ACutFallsWhereTheSpeakerPausedLongest) {
    // w0 … w59: a window from w0 holds 60 to 140 bytes when it ends after w17 to w36.
    std::vector<Word> said = words(60);
    pause_before(said, 5, Millis{2000});  // longer, but before the least a window holds
    pause_before(said, 23, Millis{900});
    pause_before(said, 40, Millis{5000});  // longer, but past the most a window may hold
    const std::vector<Window> windows = windows_of(said, kSmall);
    ASSERT_GE(windows.size(), 2U);
    EXPECT_EQ(windows[0].end, 23U);
}

TEST(Windows, WithoutPausesACutFallsNearestTheTarget) {
    const std::vector<Word> said = words(60);  // all gaps alike: 0
    const std::vector<Window> windows = windows_of(said, kSmall);
    ASSERT_GE(windows.size(), 2U);
    const std::size_t at = bytes(said, 0, windows[0].end);
    const std::size_t one_more = bytes(said, 0, windows[0].end + 1);
    const std::size_t one_less = bytes(said, 0, windows[0].end - 1);
    const auto distance = [](std::size_t length) { return length > 100 ? length - 100 : 100 - length; };
    EXPECT_LE(distance(at), distance(one_more));
    EXPECT_LE(distance(at), distance(one_less));
}

TEST(Windows, EachIsSentWithTheWordsAroundIt) {
    const std::vector<Word> said = words(100);
    const std::vector<Window> windows = windows_of(said, kSmall);
    ASSERT_GE(windows.size(), 3U);
    const Window& middle = windows[1];
    EXPECT_LT(middle.context_begin, middle.begin);
    EXPECT_LE(bytes(said, middle.context_begin, middle.begin), kSmall.context);
    EXPECT_GT(bytes(said, middle.context_begin - 1, middle.begin), kSmall.context) << "as many as fit";
    EXPECT_GT(middle.context_end, middle.end);
    EXPECT_LE(bytes(said, middle.end, middle.context_end), kSmall.context);
    EXPECT_GT(bytes(said, middle.end, middle.context_end + 1), kSmall.context);
    EXPECT_EQ(windows.front().context_begin, 0U);
    EXPECT_EQ(windows.back().context_end, said.size());
    const std::string excerpt = excerpt_of(said, windows.front());
    EXPECT_TRUE(excerpt.starts_with("w0 w1 w2 ")) << excerpt;
    EXPECT_EQ(excerpt.size(), bytes(said, 0, windows.front().context_end)) << "joined by single spaces";
}

TEST(Windows, AWordTooLongForAnyWindowIsOneOfItsOwn) {
    std::vector<Word> said = words(40);
    said[10].text = std::string(500, 'x');
    const std::vector<Window> windows = windows_of(said, kSmall);
    const auto own = std::ranges::find_if(windows, [](const Window& window) { return window.begin == 10; });
    ASSERT_NE(own, windows.end());
    EXPECT_EQ(own->end, 11U);
}

TEST(Windows, FewWordsAreOneWindowAndNoneAreNone) {
    EXPECT_THAT(windows_of(words(5), kSmall),
                ElementsAre(Window{.begin = 0, .end = 5, .context_begin = 0, .context_end = 5}));
    EXPECT_THAT(windows_of({}, kSmall), IsEmpty());
}

// --- Answers -----------------------------------------------------------------------------------

constexpr std::string_view kSaid = "so marie curie won two nobel prizes and she called the element radium";

Window whole(const std::vector<Word>& said) {
    return Window{.begin = 0, .end = said.size(), .context_begin = 0, .context_end = said.size()};
}

TEST(PunctuationAnswers, AnAnswerThatOnlyPunctuatesIsTaken) {
    const std::vector<Word> said = words(kSaid);
    EXPECT_THAT(
        punctuation_problems(said, whole(said),
                             "So, Marie Curie won two Nobel prizes, and she called the element radium."),
        IsEmpty());
}

TEST(PunctuationAnswers, AnAnswerThatRewritesIsSentBack) {
    const std::vector<Word> said = words(kSaid);
    EXPECT_THAT(
        punctuation_problems(said, whole(said), "Marie Curie won 2 Nobel prizes. She named it radium."),
        ElementsAre(HasSubstr("Keep every word exactly as it is")));
    EXPECT_THAT(
        punctuation_problems(said, whole(said),
                             "So, Marie Curie won 2 Nobel prizes, and she called the element radium."),
        IsEmpty())
        << "one word in twenty, and at least two, may differ: those are not taken anyway";
}

TEST(PunctuationAnswers, AnAnswerThatEndsNoSentenceIsSentBack) {
    const std::vector<Word> said = words(kSaid);
    EXPECT_THAT(punctuation_problems(said, whole(said), std::string{kSaid}),
                ElementsAre(HasSubstr("Mark where each sentence ends")));
}

TEST(PunctuationAnswers, AnAnswerTooLongToAlignIsSentBack) {
    const std::vector<Word> said = words(kSaid);
    std::string long_answer;
    for (int index = 0; index < 2100; ++index) {
        long_answer += "radium. ";
    }
    EXPECT_THAT(punctuation_problems(said, whole(said), long_answer), ElementsAre(HasSubstr("2048")));
}

TEST(PunctuationAnswers, OnlyTheWindowsOwnWordsAreTakenWithTheirTimes) {
    const std::vector<Word> said = words(kSaid);  // 13 words
    const Window window{.begin = 3, .end = 7, .context_begin = 1, .context_end = 10};
    ASSERT_EQ(excerpt_of(said, window), "marie curie won two nobel prizes and she called");
    const std::vector<Word> own =
        unwrap(punctuated_words(said, window, "Marie Curie won two Nobel prizes, and she called"));
    ASSERT_EQ(own.size(), 4U);
    EXPECT_EQ(own[0].text, "won");
    EXPECT_EQ(own[2].text, "Nobel");
    EXPECT_EQ(own[3].text, "prizes,");
    EXPECT_EQ(own[0].start, said[3].start);
    EXPECT_EQ(own[3].end, said[6].end);
}

}  // namespace
}  // namespace rr::pipeline

#include <rr/transcript/sentences.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>
#include <rr/transcript/captions.hpp>
#include <rr/transcript/projection.hpp>

namespace rr::transcript {
namespace {

using knowledge::Sentence;
using knowledge::Transcript;
using testkit::unwrap;
using Texts = std::vector<std::string>;

Word word(std::string text, std::int64_t start, std::int64_t end) {
    return Word{.text = std::move(text), .start = Millis{start}, .end = Millis{end}};
}

// "S1 [0-1000] Hello, everyone.": each sentence as tests compare them.
Texts sentences(const Transcript& transcript) {
    Texts found;
    for (const Sentence& sentence : transcript.sentences) {
        found.push_back(std::format("{} [{}-{}] {}", sentence.id.str(), sentence.span.start().count(),
                                    sentence.span.end().count(), sentence.text));
    }
    return found;
}

TEST(Sentences, WordsBecomeNumberedSentencesWithTheirTimes) {
    const std::vector<Word> words{word("Hello,", 0, 400),      word("everyone.", 500, 1000),
                                  word("Water", 1500, 1800),   word("boils", 1900, 2200),
                                  word("at", 2300, 2400),      word("100", 2500, 2800),
                                  word("degrees.", 2900, 3500)};
    EXPECT_EQ(sentences(transcript_of(words)),
              (Texts{"S1 [0-1000] Hello, everyone.", "S2 [1500-3500] Water boils at 100 degrees."}));
}

TEST(Sentences, ANewSpeakerStartsASentence) {
    std::vector<Word> words{word("Yes", 0, 300), word("and", 400, 600), word("so", 700, 800),
                            word("on.", 900, 1000)};
    words[0].speaker = "Ada";
    words[1].speaker = "Charles";
    words[1].paragraph = true;
    words[2].speaker = "Charles";
    words[3].speaker = "Charles";
    const Transcript transcript = transcript_of(words);
    EXPECT_EQ(sentences(transcript), (Texts{"S1 [0-300] Yes", "S2 [400-1000] and so on."}));
    EXPECT_EQ(transcript.sentences[0].speaker, "Ada");
    EXPECT_EQ(transcript.sentences[1].speaker, "Charles");
}

TEST(Sentences, ASentenceEndingInsideAWordTakesItsShareOfTheTime) {
    // Chinese needs no space after a sentence: one caption word holds two.
    EXPECT_EQ(sentences(transcript_of(
                  {word("\u4ECA\u65E5\u306F\u6674\u308C\u3002\u660E\u65E5\u306F\u96E8\u3002", 0, 2000)})),
              (Texts{"S1 [0-1090] \u4ECA\u65E5\u306F\u6674\u308C\u3002",
                     "S2 [1090-2000] \u660E\u65E5\u306F\u96E8\u3002"}))
        << "18 of its 33 bytes, then the rest";
}

TEST(Sentences, ALongSentenceIsCutWhereItPausesLongest) {
    const Texts said{"one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten"};
    std::vector<Word> words;
    for (std::size_t index = 0; index < said.size(); ++index) {
        const auto start = static_cast<std::int64_t>((200 * index) + (index >= 6 ? 1000 : 0));
        words.push_back(word(said[index], start, start + 100));
    }
    EXPECT_EQ(sentences(transcript_of(words, SentenceOptions{.max_words = 4})),
              (Texts{"S1 [0-500] one two three", "S2 [600-1100] four five six",
                     "S3 [2200-2900] seven eight nine ten"}))
        << "first at the long pause after \"six\"; then, all pauses equal, at the middle";
}

TEST(Sentences, UntimedWordsGiveUntimedSentences) {
    const CaptionTrack track = unwrap(read_captions("It rained. We stayed in.", CaptionFormat::text));
    EXPECT_EQ(sentences(transcript_of(track.words)),
              (Texts{"S1 [0-0] It rained.", "S2 [0-0] We stayed in."}));
}

TEST(Sentences, PunctuatedCaptionsEndSentencesOften) {
    std::vector<Word> words{word("Hello.", 0, 1)};
    for (int index = 1; index < 40; ++index) {
        words.push_back(word("and", 0, 1));
    }
    EXPECT_TRUE(punctuated(words)) << "one end in 40 words";
    words.push_back(word("and", 0, 1));
    EXPECT_FALSE(punctuated(words)) << "one end in 41 words";
    EXPECT_TRUE(punctuated({}));
    EXPECT_FALSE(punctuated({word("hi", 0, 1), word("there", 1, 2)}));
    EXPECT_TRUE(punctuated({word("hi", 0, 1), word("there.\u201D", 1, 2)}))
        << "a quote may close after the end";
}

TEST(Sentences, AutomaticCaptionsBecomeSentencesOnceProjected) {
    const CaptionTrack track = unwrap(read_captions(R"({"events": [
        {"tStartMs": 0, "dDurationMs": 3000, "segs": [{"utf8": "so"}, {"utf8": " marie", "tOffsetMs": 300},
            {"utf8": " curie", "tOffsetMs": 600}, {"utf8": " won", "tOffsetMs": 900}]},
        {"tStartMs": 3000, "dDurationMs": 2000, "segs": [{"utf8": "she"}, {"utf8": " left", "tOffsetMs": 500}]}]})",
                                                    CaptionFormat::json3));
    EXPECT_FALSE(punctuated(track.words));
    const Projection projection = unwrap(project(track.words, "So Marie Curie won. She left."));
    EXPECT_EQ(sentences(transcript_of(projection.words)),
              (Texts{"S1 [0-3000] So Marie Curie won.", "S2 [3000-5000] She left."}));
}

}  // namespace
}  // namespace rr::transcript

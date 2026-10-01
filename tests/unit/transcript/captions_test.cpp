#include <rr/transcript/captions.hpp>

#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>

namespace rr::transcript {
namespace {

using testkit::unwrap;
using Texts = std::vector<std::string>;

CaptionTrack read(std::string_view content, CaptionFormat format) {
    return unwrap(read_captions(content, format));
}

Texts texts(const CaptionTrack& track) {
    Texts found;
    for (const Word& word : track.words) {
        found.push_back(word.text);
    }
    return found;
}

// "hello@160-426": each word with when it was said.
Texts timed(const CaptionTrack& track) {
    Texts found;
    for (const Word& word : track.words) {
        found.push_back(std::format("{}@{}-{}", word.text, word.start.count(), word.end.count()));
    }
    return found;
}

// The words that start a paragraph.
Texts paragraphs(const CaptionTrack& track) {
    Texts found;
    for (const Word& word : track.words) {
        if (word.paragraph) {
            found.push_back(word.text);
        }
    }
    return found;
}

std::string error_of(std::string_view content, CaptionFormat format) {
    const Result<CaptionTrack> read = read_captions(content, format);
    EXPECT_FALSE(read.has_value()) << content;
    return read.has_value() ? std::string{} : read.error().message();
}

TEST(Captions, AFileIsKnownByItsExtension) {
    EXPECT_EQ(format_of("talk.en.json3"), CaptionFormat::json3);
    EXPECT_EQ(format_of("talk.VTT"), CaptionFormat::vtt);
    EXPECT_EQ(format_of("talk.srt"), CaptionFormat::srt);
    EXPECT_EQ(format_of("notes.txt"), CaptionFormat::text);
    EXPECT_EQ(format_of("talk.mp4"), std::nullopt);
    EXPECT_EQ(format_of("README"), std::nullopt);
}

TEST(Captions, Json3TimesEveryWordOfAutomaticCaptions) {
    const CaptionTrack track = read(R"({"wireMagic": "pb3", "events": [
        {"tStartMs": 0, "dDurationMs": 6000, "id": 1, "wpWinPosId": 1, "wsWinStyleId": 1},
        {"tStartMs": 160, "dDurationMs": 4000, "wWinId": 1, "segs": [
            {"utf8": "so", "acAsrConf": 0}, {"utf8": " today", "tOffsetMs": 400}, {"utf8": " we", "tOffsetMs": 800}]},
        {"tStartMs": 2550, "dDurationMs": 10, "wWinId": 1, "aAppend": 1, "segs": [{"utf8": "\n"}]},
        {"tStartMs": 2560, "dDurationMs": 3000, "wWinId": 1, "segs": [
            {"utf8": "talk"}, {"utf8": " about", "tOffsetMs": 300}]}]})",
                                    CaptionFormat::json3);
    EXPECT_EQ(timed(track),
              (Texts{"so@160-426", "today@560-893", "we@960-2560", "talk@2560-2800", "about@2860-5560"}))
        << "a word ends where the next starts, at the latest";
    EXPECT_TRUE(track.timed);
}

TEST(Captions, AWordWithoutItsOwnTimeSharesItsCues) {
    const CaptionTrack track = read(
        R"({"events": [{"tStartMs": 1000, "dDurationMs": 2000, "segs": [{"utf8": "Hello, everyone."}]}]})",
        CaptionFormat::json3);
    EXPECT_EQ(timed(track), (Texts{"Hello,@1000-1750", "everyone.@1875-3000"}))
        << "by their length in the cue";
}

TEST(Captions, Json3ThatIsNotSaysWhere) {
    EXPECT_EQ(error_of("[1, 2]", CaptionFormat::json3),
              "not json3: json3 captions are an object with a list of \"events\"");
    EXPECT_EQ(error_of(R"({"events": [{"id": 1}, {"segs": [{"utf8": "hi"}]}]})", CaptionFormat::json3),
              "json3 event 2 has no \"tStartMs\"");
    EXPECT_EQ(error_of(R"({"events": [{"tStartMs": -5, "segs": [{"utf8": "hi"}]}]})", CaptionFormat::json3),
              "json3 event 1: \"tStartMs\" is -5, not milliseconds");
    EXPECT_NE(error_of(R"({"events": [{"tStartMs": 1e300, "segs": [{"utf8": "hi"}]}]})", CaptionFormat::json3)
                  .find("not milliseconds"),
              std::string::npos)
        << "no video lasts that long";
    EXPECT_EQ(error_of(R"({"events": [{"tStartMs": 0, "segs": [{"text": "hi"}]}]})", CaptionFormat::json3),
              "json3 event 1: a segment without \"utf8\" text");
    EXPECT_FALSE(read_captions("{not json", CaptionFormat::json3).has_value());
}

TEST(Captions, WebVttCuesAreReadWithTheirVoices) {
    const CaptionTrack track = read(
        "WEBVTT\nKind: captions\n\nNOTE a comment\nthat spans lines\n\nintro\n"
        "00:00:01.000 --> 00:00:03.000 align:start position:0%\n<v Ada>Hello, <i>everyone</i>.</v>\n\n"
        "00:00:03.500 --> 00:00:05.000\nFish &amp; chips.\n",
        CaptionFormat::vtt);
    EXPECT_EQ(texts(track), (Texts{"Hello,", "everyone.", "Fish", "&", "chips."}));
    EXPECT_EQ(track.words[0].speaker, "Ada");
    EXPECT_EQ(track.words[1].speaker, "Ada");
    EXPECT_EQ(track.words[2].speaker, std::nullopt) << "a voice speaks only what it wraps";
    EXPECT_EQ(paragraphs(track), Texts{"Fish"}) << "the speaker changes";
    EXPECT_EQ(timed(track)[0], "Hello,@1000-1750");
}

TEST(Captions, AVoiceSpeaksOnlyWhatItWraps) {
    const CaptionTrack track =
        read("WEBVTT\n\n00:00:01.000 --> 00:00:02.000\n<v Ada>Hi,</v> said someone.\n", CaptionFormat::vtt);
    EXPECT_EQ(track.words[0].speaker, "Ada");
    EXPECT_EQ(track.words[1].speaker, std::nullopt);
}

TEST(Captions, RollingCaptionsAreReadOnce) {
    const CaptionTrack track = read(
        "WEBVTT\nKind: captions\nLanguage: en\n\n"
        "00:00:00.160 --> 00:00:02.869 align:start position:0%\n \n"
        "hello<00:00:00.480><c> and</c><00:00:00.960><c> welcome</c>\n\n"
        "00:00:02.869 --> 00:00:02.879 align:start position:0%\nhello and welcome\n \n\n"
        "00:00:02.879 --> 00:00:05.990 align:start position:0%\nhello and welcome\n"
        "to<00:00:03.120><c> the</c><00:00:03.400><c> show</c>\n",
        CaptionFormat::vtt);
    EXPECT_EQ(timed(track), (Texts{"hello@160-426", "and@480-840", "welcome@960-2869", "to@2879-3039",
                                   "the@3120-3330", "show@3400-5990"}));
}

TEST(Captions, AByteOrderMarkIsNoText) {
    EXPECT_EQ(texts(read("\xEF\xBB\xBFWEBVTT\n\n00:00:01.000 --> 00:00:02.000\nHi\n", CaptionFormat::vtt)),
              Texts{"Hi"});
}

TEST(Captions, AHeaderRunningIntoItsFirstCueKeepsIt) {
    EXPECT_EQ(texts(read("WEBVTT\n00:00:01.000 --> 00:00:02.000\nHi there.\n", CaptionFormat::vtt)),
              (Texts{"Hi", "there."}));
}

TEST(Captions, WebVttThatIsNotSaysWhere) {
    EXPECT_EQ(error_of("WEBVTTX\n", CaptionFormat::vtt), "not WebVTT: its first line is not \"WEBVTT\"");
    EXPECT_EQ(error_of("", CaptionFormat::vtt), "not WebVTT: its first line is not \"WEBVTT\"");
    EXPECT_EQ(error_of("WEBVTT\n\nintro\nHello\n", CaptionFormat::vtt),
              "line 4: a cue without its timing (\"00:00:01.000 --> 00:00:04.000\")");
    EXPECT_EQ(error_of("WEBVTT\n\n00:00:01.000 --> soon\nHi\n", CaptionFormat::vtt),
              "line 3: \"00:00:01.000 --> soon\" is no cue timing");
    EXPECT_EQ(error_of("WEBVTT\n\n00:00:01.5 --> 00:00:02.000\nHi\n", CaptionFormat::vtt),
              "line 3: \"00:00:01.5 --> 00:00:02.000\" is no cue timing")
        << "milliseconds are written in three digits";
    EXPECT_EQ(error_of("WEBVTT\n\n300:00:00.000 --> 300:00:01.000\nHi\n", CaptionFormat::vtt),
              "line 3: \"300:00:00.000 --> 300:00:01.000\" is no cue timing")
        << "no video lasts 12 days";
    EXPECT_EQ(error_of("WEBVTT\n\n99999999999999999:00:01.000 --> 00:00:02.000\nHi\n", CaptionFormat::vtt),
              "line 3: \"99999999999999999:00:01.000 --> 00:00:02.000\" is no cue timing");
}

TEST(Captions, SubRipCuesAreReadWithoutTheirStyles) {
    const CaptionTrack track = read(
        "1\r\n00:00:01,000 --> 00:00:02,000\r\n<i>Hello,</i> {\\an8}everyone. <3\r\n\r\n"
        "2\r\n00:00:02,500 --> 00:00:04,000\r\n- How are you?\r\n- Fine, thanks.\r\n",
        CaptionFormat::srt);
    EXPECT_EQ(texts(track), (Texts{"Hello,", "everyone.", "<3", "How", "are", "you?", "Fine,", "thanks."}));
    EXPECT_EQ(paragraphs(track), (Texts{"How", "Fine,"})) << "each dash, a new speaker";
}

TEST(Captions, SubRipThatIsNotSaysWhere) {
    EXPECT_EQ(texts(read("00:00:01,000 --> 00:00:02,000\nNo number.\n", CaptionFormat::srt)),
              (Texts{"No", "number."}))
        << "a cue's number may be left out";
    EXPECT_EQ(error_of("1\nnot a timing\nHi\n", CaptionFormat::srt),
              "line 2: \"not a timing\" is no cue timing (\"00:00:01,000 --> 00:00:04,000\")");
    EXPECT_EQ(error_of("1\n", CaptionFormat::srt),
              "line 1: \"1\" is no cue timing (\"00:00:01,000 --> 00:00:04,000\")");
}

TEST(Captions, PlainTextIsUntimedParagraphs) {
    const CaptionTrack track =
        read("First paragraph here.\nStill the first.\n\n\nSecond one.\n", CaptionFormat::text);
    EXPECT_EQ(texts(track),
              (Texts{"First", "paragraph", "here.", "Still", "the", "first.", "Second", "one."}));
    EXPECT_EQ(paragraphs(track), (Texts{"First", "Second"}));
    EXPECT_FALSE(track.timed);
    EXPECT_EQ(timed(track).back(), "one.@0-0");
}

TEST(Captions, SoundsAreSetApartFromWhatWasSaid) {
    const CaptionTrack track = read(
        "WEBVTT\n\n00:00:01.000 --> 00:00:03.000\n[Music] \u266A la la \u266A (laughs) [inaudible] "
        "ok(sighs)\n",
        CaptionFormat::vtt);
    EXPECT_EQ(texts(track), (Texts{"la", "la", "[unclear]", "ok"}))
        << "music notes are not words; a word missed stays a gap";
    ASSERT_EQ(track.sounds.size(), 3U);
    EXPECT_EQ(track.sounds[0].text, "[Music]");
    EXPECT_EQ(track.sounds[1].text, "(laughs)");
    EXPECT_EQ(track.sounds[2].text, "(sighs)");
    EXPECT_EQ(texts(read("WEBVTT\n\n00:00:01.000 --> 00:00:03.000\n[an unclosed bracket\nruns] no further\n",
                         CaptionFormat::vtt)),
              (Texts{"[an", "unclosed", "bracket", "runs]", "no", "further"}))
        << "a bracket never closed on its line is text";
}

TEST(Captions, ACaptionersLabelNamesTheSpeaker) {
    const CaptionTrack track =
        read("ADA LOVELACE: Numbers are poetry.\n>> And who are you?\nAda: no label.\n", CaptionFormat::text);
    EXPECT_EQ(texts(track),
              (Texts{"Numbers", "are", "poetry.", "And", "who", "are", "you?", "Ada:", "no", "label."}));
    EXPECT_EQ(track.words[0].speaker, "ADA LOVELACE");
    EXPECT_EQ(track.words[2].speaker, "ADA LOVELACE");
    EXPECT_EQ(track.words[3].speaker, std::nullopt) << "\">>\": a new speaker, not named";
    EXPECT_EQ(paragraphs(track), (Texts{"Numbers", "And"}));
    EXPECT_EQ(texts(read("I: think so.\n", CaptionFormat::text)), (Texts{"I:", "think", "so."}))
        << "a label holds two capitals at least";
}

TEST(Captions, TextIsKeptValidAndComposed) {
    const CaptionTrack track = read("cafe\u0301 \xFF ok", CaptionFormat::text);
    EXPECT_EQ(texts(track), (Texts{"caf\u00E9", "\uFFFD", "ok"}));
}

TEST(Captions, ControlsSeparateWordsAndJoinersJoinThem) {
    using std::string_literals::operator""s;
    const CaptionTrack track = read("a\0b \u200Bc d\u200Ce \u200Df\u200D"s, CaptionFormat::text);
    EXPECT_EQ(texts(track), (Texts{"a", "b", "c", "d\u200Ce", "f"}))
        << "a NUL byte or a zero-width space is no part of a word; a joiner within one is";
}

TEST(Captions, CharacterReferencesAreDecoded) {
    EXPECT_EQ(
        texts(read("WEBVTT\n\n00:00:01.000 --> 00:00:03.000\nFish &amp; chips &#39;n&#x2019; &bogus; &#0;\n",
                   CaptionFormat::vtt)),
        (Texts{"Fish", "&", "chips", "'n\u2019", "&bogus;", "&#0;"}))
        << "unknown references stay as written";
}

TEST(Captions, AWordNeverStartsBeforeTheOneBefore) {
    const CaptionTrack track =
        read("1\n00:00:00,000 --> 00:00:04,000\na b c d\n\n2\n00:00:01,500 --> 00:00:02,000\nx\n",
             CaptionFormat::srt);
    EXPECT_EQ(timed(track), (Texts{"a@0-571", "b@1142-1714", "c@2285-2857", "d@3428-3428", "x@3428-3428"}))
        << "a cue shown over another: its words come after";
}

TEST(Captions, WordsAreReadInTheOrderSaid) {
    const CaptionTrack track = read(
        "2\n00:00:05,000 --> 00:00:06,000\nlater\n\n1\n00:00:01,000 --> 00:00:02,000\nearlier\n\n"
        "3\n00:00:04,000 --> 00:00:04,500\noverlapping\n",
        CaptionFormat::srt);
    EXPECT_EQ(timed(track), (Texts{"earlier@1000-2000", "overlapping@4000-4500", "later@5000-6000"}));
}

}  // namespace
}  // namespace rr::transcript

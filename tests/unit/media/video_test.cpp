#include <rr/media/video.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>

namespace rr::media {
namespace {

using testkit::unwrap;
using transcript::CaptionFormat;

Video video_of(std::string_view info) {
    return unwrap(video_from_ytdlp(unwrap(json::parse(info))));
}

CaptionOffer offer(std::string language, CaptionKind kind,
                   std::vector<std::string> formats = {"json3", "vtt"}) {
    return CaptionOffer{.language = std::move(language), .kind = kind, .formats = std::move(formats)};
}

constexpr CaptionKind kManual = CaptionKind::manual;
constexpr CaptionKind kAutomatic = CaptionKind::automatic;

TEST(Video, IsReadFromYtDlpsDescription) {
    const Video video = video_of(R"json({
        "id": "abc123XYZ_0", "_type": "video", "title": "On radium", "description": "A talk.",
        "channel": "Ada's Channel", "uploader": "ada", "upload_date": "20240131", "duration": 1234.5,
        "language": "en", "license": "Creative Commons Attribution license (reuse allowed)",
        "webpage_url": "https://www.youtube.com/watch?v=abc123XYZ_0", "tags": ["science", 7, "radium"],
        "chapters": [{"start_time": 0, "end_time": 60.5, "title": "Intro"},
                     {"start_time": 60.5, "end_time": 30, "title": "Broken"},
                     {"start_time": 60.5, "end_time": 1234.5, "title": "Radium"}],
        "subtitles": {"en-GB": [{"ext": "json3", "url": "u"}, {"ext": "vtt"}], "live_chat": [{"ext": "json"}]},
        "automatic_captions": {"en-orig": [{"ext": "json3"}], "en": [{"ext": "json3"}], "fr": [{"ext": "srv3"}]},
        "is_live": false, "live_status": "not_live"})json");
    EXPECT_EQ(video.id, "abc123XYZ_0");
    EXPECT_EQ(video.url, "https://www.youtube.com/watch?v=abc123XYZ_0");
    EXPECT_EQ(video.title, "On radium");
    EXPECT_EQ(video.description, "A talk.");
    EXPECT_EQ(video.channel, "Ada's Channel");
    EXPECT_EQ(video.upload_date, "2024-01-31");
    EXPECT_EQ(video.duration, Millis{1'234'500});
    EXPECT_EQ(video.language, "en");
    EXPECT_EQ(video.license, "Creative Commons Attribution license (reuse allowed)");
    EXPECT_EQ(video.tags, (std::vector<std::string>{"science", "radium"}));
    ASSERT_EQ(video.chapters.size(), 2U) << "a chapter ending before it starts is left out";
    EXPECT_EQ(video.chapters[1].title, "Radium");
    EXPECT_EQ(video.chapters[1].span, unwrap(TimeSpan::make(Millis{60'500}, Millis{1'234'500})));
    EXPECT_EQ(video.captions,
              (std::vector<CaptionOffer>{offer("en-GB", kManual), offer("en", kAutomatic, {"json3"}),
                                         offer("en-orig", kAutomatic, {"json3"}),
                                         offer("fr", kAutomatic, {"srv3"})}))
        << "a live chat is no captions";
    EXPECT_FALSE(video.live);
}

std::string error_of(std::string_view info) {
    const Result<Video> video = video_from_ytdlp(unwrap(json::parse(info)));
    EXPECT_FALSE(video.has_value()) << info;
    return video.has_value() ? std::string{} : video.error().message();
}

TEST(Video, IsOneVideoWithAnId) {
    EXPECT_EQ(error_of(R"({"_type": "playlist", "id": "PL1"})"),
              "the address is a playlist, not a video: give the address of one video");
    EXPECT_EQ(error_of(R"({"title": "untitled"})"), "yt-dlp gave the video no ID");
    EXPECT_EQ(error_of("[]"), "yt-dlp described no video");
}

TEST(Video, WhatIsNotToldIsLeftOut) {
    const Video video = video_of(
        R"({"id": "x", "duration": -3, "language": null, "chapters": null, "upload_date": "2024", "uploader": "ada"})");
    EXPECT_EQ(video.duration, std::nullopt);
    EXPECT_EQ(video.language, std::nullopt);
    EXPECT_TRUE(video.chapters.empty());
    EXPECT_EQ(video.upload_date, std::nullopt);
    EXPECT_EQ(video.channel, "ada") << "the uploader, when there is no channel";
    EXPECT_EQ(video_of(R"({"id": "x", "upload_date": "2024Jan1"})").upload_date, std::nullopt);
}

TEST(Video, ALiveStreamHasNoCaptionsYet) {
    EXPECT_TRUE(video_of(R"({"id": "x", "is_live": true})").live);
    const Video upcoming = video_of(R"({"id": "x", "live_status": "is_upcoming"})");
    EXPECT_TRUE(upcoming.live);
    EXPECT_EQ(choose_captions(upcoming).error().code(), Errc::unavailable);
}

TEST(CaptionChoice, TheCreatorsCaptionsInTheLanguageSpokenComeFirst) {
    const Video video{
        .language = "en",
        .captions = {offer("en", kManual), offer("en", kAutomatic), offer("en-orig", kAutomatic)}};
    EXPECT_EQ(unwrap(choose_captions(video)),
              (CaptionChoice{.language = "en", .kind = kManual, .format = CaptionFormat::json3}));
}

TEST(CaptionChoice, ARegionalVariantIsTheLanguageToo) {
    const Video video{.language = "en", .captions = {offer("en-GB", kManual), offer("en-orig", kAutomatic)}};
    EXPECT_EQ(unwrap(choose_captions(video)).language, "en-GB");
    const Video both{.language = "en-GB", .captions = {offer("en", kManual), offer("en-GB", kManual)}};
    EXPECT_EQ(unwrap(choose_captions(both)).language, "en-GB") << "the very language first";
}

TEST(CaptionChoice, ElseTheSpeechRecognitionsOwnTrack) {
    const Video video{
        .language = "en",
        .captions = {offer("fr", kManual), offer("en-orig", kAutomatic), offer("fr", kAutomatic)}};
    EXPECT_EQ(unwrap(choose_captions(video)),
              (CaptionChoice{.language = "en-orig", .kind = kAutomatic, .format = CaptionFormat::json3}))
        << "never the creator's translation";
}

TEST(CaptionChoice, TheLanguageSpokenIsTheOneRecognized) {
    const Video video{.captions = {offer("en", kManual), offer("de", kManual), offer("de-orig", kAutomatic)}};
    EXPECT_EQ(unwrap(choose_captions(video)).language, "de");
}

TEST(CaptionChoice, NeverATranslation) {
    const Video video{.language = "es", .captions = {offer("en", kManual), offer("en", kAutomatic)}};
    const Result<CaptionChoice> choice = choose_captions(video);
    ASSERT_FALSE(choice.has_value());
    EXPECT_EQ(choice.error().code(), Errc::not_found);
    EXPECT_EQ(
        choice.error().message(),
        "no captions in the language spoken (es); on offer: en (the creator's), en (automatic). The video "
        "needs speech recognition (milestone M4)");
}

TEST(CaptionChoice, ALanguageNeverToldLeavesTheCreatorsOnlyTrack) {
    EXPECT_EQ(unwrap(choose_captions(Video{.captions = {offer("en", kManual)}})).language, "en");
    const Result<CaptionChoice> two =
        choose_captions(Video{.captions = {offer("en", kManual), offer("fr", kManual)}});
    ASSERT_FALSE(two.has_value()) << "which of two is what was said is not told";
    EXPECT_EQ(two.error().message(),
              "no captions in the language spoken (not told); on offer: en (the creator's), fr (the "
              "creator's). The "
              "video needs speech recognition (milestone M4)");
}

TEST(CaptionChoice, Json3IsReadFirstThenWebVttThenSubRip) {
    EXPECT_EQ(unwrap(choose_captions(
                         Video{.language = "en", .captions = {offer("en", kManual, {"srv3", "srt", "vtt"})}}))
                  .format,
              CaptionFormat::vtt);
    EXPECT_EQ(
        unwrap(choose_captions(Video{.language = "en", .captions = {offer("en", kManual, {"ttml", "srt"})}}))
            .format,
        CaptionFormat::srt);
    const Video unreadable{
        .language = "en",
        .captions = {offer("en", kManual, {"srv3"}), offer("en-orig", kAutomatic, {"json3"})}};
    EXPECT_EQ(unwrap(choose_captions(unreadable)).language, "en-orig")
        << "a track rReport cannot read is passed by";
}

TEST(CaptionChoice, AnOlderYtDlpNamesTheRecognizedTrackByItsLanguage) {
    EXPECT_EQ(unwrap(choose_captions(
                  Video{.language = "en", .captions = {offer("de", kAutomatic), offer("en", kAutomatic)}})),
              (CaptionChoice{.language = "en", .kind = kAutomatic, .format = CaptionFormat::json3}));
}

}  // namespace
}  // namespace rr::media

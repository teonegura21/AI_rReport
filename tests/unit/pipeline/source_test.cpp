#include <rr/pipeline/source.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/core/file_io.hpp>
#include <rr/core/hash.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

namespace rr::pipeline {
namespace {

using ::testing::HasSubstr;
using testkit::unwrap;
using transcript::CaptionFormat;

constexpr std::string_view kUrl = "https://www.youtube.com/watch?v=abc123XYZ_0";
constexpr std::string_view kVtt = "WEBVTT\n\n00:00.000 --> 00:02.000\nRadium glows.\n";

// The description yt-dlp gives of the video at kUrl.
constexpr std::string_view kInfo = R"json({
  "id": "abc123XYZ_0", "title": "On radium", "description": "A lecture on Marie Curie's work.",
  "channel": "Science Hour", "upload_date": "20091025", "duration": 754, "language": "en",
  "license": "Creative Commons Attribution license (reuse allowed)", "tags": ["radium", ""],
  "webpage_url": "https://www.youtube.com/watch?v=abc123XYZ_0",
  "chapters": [{"title": "Polonium", "start_time": 0, "end_time": 60}],
  "subtitles": {"en": [{"ext": "vtt"}]},
  "automatic_captions": {"en-orig": [{"ext": "json3"}]}})json";

// yt-dlp, played by a script: it describes the video with `info`, and
// downloads the captions kVtt.
class FakeYtDlp {
public:
    explicit FakeYtDlp(std::string_view info) {
        (void)directory_.write_file("info.json", info);
        (void)directory_.write_file("captions.vtt", kVtt);
        script_ = directory_.write_file("yt-dlp", std::format(R"(#!/bin/sh
mode=captions
while [ $# -gt 0 ]; do
  case "$1" in
    --dump-single-json) mode=describe ;;
    --paths) directory="$2" ;;
  esac
  shift
done
if [ "$mode" = describe ]; then cat '{0}/info.json'; else cp '{0}/captions.vtt' "$directory/captions.en.vtt"; fi
)",
                                                              directory_.path().string()));
        std::filesystem::permissions(script_, std::filesystem::perms::owner_all);
    }

    [[nodiscard]] media::YtDlp ytdlp() const {
        return media::YtDlp{
            media::YtDlpOptions{.executable = script_.string(), .timeout = std::chrono::seconds{20}}};
    }

private:
    testkit::TempDir directory_{"rr_source"};
    std::filesystem::path script_;
};

class SourceTest : public ::testing::Test {
protected:
    // The source stage's fingerprint and output for a job of `source`.
    Result<StageOutput> run(const SourceStage& stage, const std::string& source) {
        job_.source = source;
        StageContext context{.job = &job_, .work_directory = dir_ / "jobs" / "J1"};
        RR_TRY_ASSIGN(context.fingerprint, stage.fingerprint(context));
        return stage.run(context);
    }

    [[nodiscard]] testkit::TempDir& dir() { return dir_; }
    [[nodiscard]] library::Job& job() { return job_; }

private:
    testkit::TempDir dir_;
    library::Job job_{.id = "J1"};
};

TEST_F(SourceTest, AVideoIsReadWithItsCaptionsInTheLanguageSpoken) {
    const FakeYtDlp fake{kInfo};
    const SourceStage stage{fake.ytdlp()};
    job().source = std::string{kUrl};
    EXPECT_EQ(unwrap(stage.fingerprint(StageContext{.job = &job()})), (json::Json{{"address", kUrl}}))
        << "a video is read once per library";

    const StageOutput output = unwrap(run(stage, std::string{kUrl}));
    EXPECT_TRUE(output.findings.empty());
    const Source source = unwrap(source_from_json(output.artifact));
    EXPECT_EQ(source.origin, Origin::web);
    EXPECT_EQ(source.video.id, "abc123XYZ_0");
    EXPECT_EQ(source.video.url, kUrl);
    EXPECT_EQ(source.video.title, "On radium");
    EXPECT_EQ(source.video.description, "A lecture on Marie Curie's work.");
    EXPECT_EQ(source.video.channel, "Science Hour");
    EXPECT_EQ(source.video.upload_date, "2009-10-25");
    EXPECT_EQ(source.video.duration, std::chrono::milliseconds{754'000});
    EXPECT_EQ(source.video.license, "Creative Commons Attribution license (reuse allowed)");
    EXPECT_EQ(source.video.tags, std::vector<std::string>{"radium"}) << "an empty tag says nothing";
    ASSERT_EQ(source.video.chapters.size(), 1U);
    EXPECT_EQ(source.video.chapters[0].title, "Polonium");
    EXPECT_EQ(source.video.chapters[0].span.end(), std::chrono::milliseconds{60'000});
    EXPECT_TRUE(source.video.captions.empty()) << "the offers are not kept: the captions read are";
    EXPECT_EQ(source.captions, (SourceCaptions{.language = "en",
                                               .kind = media::CaptionKind::manual,
                                               .format = CaptionFormat::vtt,
                                               .content = std::string{kVtt}}))
        << "the creator's captions, in the language spoken";
}

TEST_F(SourceTest, AVideoWithoutATitleIsTitledByItsId) {
    std::string info{kInfo};
    info.replace(info.find(R"("title": "On radium")"), std::string_view{R"("title": "On radium")"}.size(),
                 R"("title": "")");
    const FakeYtDlp fake{info};
    const Source source =
        unwrap(source_from_json(unwrap(run(SourceStage{fake.ytdlp()}, std::string{kUrl})).artifact));
    EXPECT_EQ(source.video.title, "abc123XYZ_0") << "the schema needs a title";
}

TEST_F(SourceTest, ALiveVideoStopsTheJobUntilItEnds) {
    std::string info{kInfo};
    info.insert(info.size() - 1, R"(, "live_status": "is_live")");
    const FakeYtDlp fake{info};
    const Result<StageOutput> output = run(SourceStage{fake.ytdlp()}, std::string{kUrl});
    EXPECT_ERRC(output, Errc::unavailable) << "a resume reads it once it has ended";
    EXPECT_THAT(output.error().message(), HasSubstr("once it has ended"));
}

TEST_F(SourceTest, ACaptionFileIsKeyedByItsNameAndItsBytes) {
    const std::filesystem::path file = dir().write_file("On radium.vtt", kVtt);
    const SourceStage stage;
    job().source = file.string();
    const json::Json fingerprint = unwrap(stage.fingerprint(StageContext{.job = &job()}));
    EXPECT_EQ(fingerprint, (json::Json{{"file", "On radium.vtt"}, {"content", blake3(kVtt).hex()}}))
        << "not its directory: a copy elsewhere is the same source";

    const Source source = unwrap(source_from_json(unwrap(run(stage, file.string())).artifact));
    EXPECT_EQ(source.origin, Origin::file);
    EXPECT_EQ(source.video.title, "On radium") << "its name, without the extension";
    EXPECT_EQ(source.video.id, "");
    EXPECT_EQ(source.captions, (SourceCaptions{.format = CaptionFormat::vtt, .content = std::string{kVtt}}))
        << "nothing is known of who wrote them, nor in what language";
}

TEST_F(SourceTest, ASavedSourceIsKeptAsItIs) {
    const FakeYtDlp fake{kInfo};
    const json::Json fetched = unwrap(run(SourceStage{fake.ytdlp()}, std::string{kUrl})).artifact;
    const std::filesystem::path saved = dir().write_file("on-radium.source.json", json::dump_pretty(fetched));
    const json::Json read = unwrap(run(SourceStage{}, saved.string())).artifact;
    EXPECT_EQ(read, fetched) << "the stages after it run as they would on the video";
    EXPECT_EQ(json::content_hash(read), json::content_hash(fetched));

    const std::filesystem::path broken = dir().write_file("broken.source.json", R"({"origin": "web"})");
    EXPECT_ERRC(run(SourceStage{}, broken.string()), Errc::invalid_argument) << "a saved source must be one";
}

TEST_F(SourceTest, AFileMustSayItsFormatAndBeThere) {
    const SourceStage stage;
    const std::filesystem::path notes = dir().write_file("notes.md", "Radium glows.");
    EXPECT_ERRC(run(stage, notes.string()), Errc::invalid_argument);
    EXPECT_ERRC(run(stage, (dir() / "missing.vtt").string()), Errc::not_found);
}

TEST_F(SourceTest, AFileThatChangesWhileItIsReadIsNotKept) {
    const std::filesystem::path file =
        dir().write_file("talk.srt", "1\n00:00:00,000 --> 00:00:01,000\nOne.\n");
    const SourceStage stage;
    job().source = file.string();
    StageContext context{.job = &job()};
    context.fingerprint = unwrap(stage.fingerprint(context));
    ASSERT_OK(write_file_atomically(file, "1\n00:00:00,000 --> 00:00:01,000\nTwo.\n"));
    EXPECT_ERRC(stage.run(context), Errc::conflict) << "its key would name bytes it does not hold";
}

TEST(Source, IsWrittenAndReadBackAsItsSchemaSays) {
    const Source source{
        .origin = Origin::web,
        .video = media::Video{.id = "abc123XYZ_0",
                              .url = std::string{kUrl},
                              .title = "On radium",
                              .description = "",
                              .channel = "Science Hour",
                              .duration = std::chrono::milliseconds{754'000},
                              .language = "en",
                              .chapters = {media::Chapter{
                                  .title = "",
                                  .span = unwrap(TimeSpan::make(std::chrono::milliseconds{0},
                                                                std::chrono::milliseconds{10}))}}},
        .captions = SourceCaptions{.language = "en-orig",
                                   .kind = media::CaptionKind::automatic,
                                   .format = CaptionFormat::json3,
                                   .content = R"({"events": []})"}};
    const json::Json written = to_json(source);
    EXPECT_TRUE(written.at("video").at("license").is_null()) << "what is not known is null";
    EXPECT_TRUE(written.at("video").at("upload_date").is_null());
    EXPECT_EQ(written.at("captions").at("kind"), "automatic");
    EXPECT_EQ(unwrap(source_from_json(written)), source);

    json::Json broken = written;
    broken["captions"]["format"] = "ttml";
    EXPECT_ERRC(source_from_json(broken), Errc::invalid_argument);
    broken = written;
    broken["video"]["chapters"][0]["end_ms"] = -1;
    EXPECT_ERRC(source_from_json(broken), Errc::invalid_argument);
    broken = written;
    broken["video"]["chapters"][0]["start_ms"] = 20;
    EXPECT_ERRC(source_from_json(broken), Errc::invalid_argument) << "a chapter that ends before it starts";
}

TEST(Source, AWebAddressIsOneOfHttpsOrHttp) {
    EXPECT_TRUE(is_web_address("https://youtu.be/abc123XYZ_0"));
    EXPECT_TRUE(is_web_address("HTTP://example.com/talk"));
    EXPECT_FALSE(is_web_address("talk.vtt"));
    EXPECT_FALSE(is_web_address("/home/ada/https.vtt"));
    EXPECT_FALSE(is_web_address("ftp://example.com/talk.vtt"));
    EXPECT_FALSE(is_web_address("https:/"));
}

}  // namespace
}  // namespace rr::pipeline

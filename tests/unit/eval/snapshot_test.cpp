// Snapshots: a video read once, through the source stage, and kept with the
// eval, pinned by its hash.
#include <rr/eval/snapshot.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/core/strings.hpp>
#include <rr/library/jobs.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

#include "fixtures.hpp"

namespace rr::eval {
namespace {

using ::testing::HasSubstr;
using namespace std::chrono_literals;
using testkit::unwrap;

constexpr std::string_view kVtt = "WEBVTT\n\n00:00.000 --> 00:02.000\nRadium glows.\n";

// When the tests fetch: 21:40:00.750 on 1 October 2026, UTC.
const std::chrono::system_clock::time_point kFetched =
    std::chrono::sys_days{std::chrono::year{2026} / 10 / 1} + 21h + 40min + 750ms;

// What yt-dlp says of the video at fixtures::kUrl: manual English captions,
// under `license`.
std::string info(std::string_view license = fixtures::kCreativeCommons) {
    return std::format(R"json({{
  "id": "abc123XYZ_0", "title": "On radium", "description": "A lecture on Marie Curie's work.",
  "channel": "Science Hour", "language": "en", "license": "{}",
  "webpage_url": "{}", "subtitles": {{"en": [{{"ext": "vtt"}}]}}}})json",
                       license, fixtures::kUrl);
}

// yt-dlp, played by a script, as in the source stage's own tests: it
// describes the video with `description`, and downloads kVtt as its
// captions.
class FakeYtDlp {
public:
    explicit FakeYtDlp(std::string_view description) {
        (void)directory_.write_file("info.json", description);
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

    [[nodiscard]] pipeline::SourceStage stage() const {
        return pipeline::SourceStage{media::YtDlp{
            media::YtDlpOptions{.executable = script_.string(), .timeout = std::chrono::seconds{20}}}};
    }

private:
    testkit::TempDir directory_{"rr_eval_ytdlp"};
    std::filesystem::path script_;
};

class SnapshotTest : public ::testing::Test {
protected:
    // Keeps the source fixtures::source_of() describes as `video`'s snapshot.
    Result<Snapshot> keep(const EvalVideo& video,
                          const pipeline::Source& source = fixtures::source_of(std::string{kVtt})) {
        return keep_snapshot(root_.path(), video, to_json(source), kFetched);
    }

    [[nodiscard]] const testkit::TempDir& root() const { return root_; }
    [[nodiscard]] std::filesystem::path file(const EvalVideo& video) const {
        return snapshot_file(root_.path(), video.id);
    }

private:
    testkit::TempDir root_{"rr_eval"};
};

TEST_F(SnapshotTest, AFetchKeepsWhatTheSourceStageReadReadablyAndPinnedByItsHash) {
    const FakeYtDlp fake{info()};
    const EvalVideo video = fixtures::video();
    const Snapshot snapshot =
        unwrap(fetch_snapshot(root().path(), video, fake.stage(), root() / "work", {}, kFetched));
    EXPECT_EQ(snapshot.source.video.title, "On radium");
    EXPECT_EQ(snapshot.source.captions.content, kVtt);
    EXPECT_EQ(snapshot.source.captions.kind, media::CaptionKind::manual);
    EXPECT_EQ(snapshot.fetched, "2026-10-01T21:40:00Z") << "to the second";

    EXPECT_EQ(file(video), root() / "data" / "radium-talk" / "radium-talk.source.json");
    const std::string written = root().read_file(file(video));
    EXPECT_EQ(snapshot.hash, json::content_hash(unwrap(json::parse(written))).hex())
        << "the artifact's own hash, as a library would key it";
    EXPECT_THAT(written, HasSubstr("\n  \"captions\": {\n")) << "written for people to read";
    const std::string record = root().read_file("data/radium-talk/snapshot.toml");
    EXPECT_THAT(record, HasSubstr(std::format("hash = \"{}\"\n", snapshot.hash)));
    EXPECT_THAT(record, HasSubstr("fetched = \"2026-10-01T21:40:00Z\"\n"));
    EXPECT_THAT(record, HasSubstr(std::format("url = \"{}\"\n", fixtures::kUrl)));

    EXPECT_EQ(unwrap(read_snapshot(root().path(), video)), snapshot) << "read back as it was kept";
}

TEST_F(SnapshotTest, AFetchThatFailsKeepsNothing) {
    const FakeYtDlp fake{
        replace_all(info(), R"("subtitles": {"en": [{"ext": "vtt"}]})", R"("subtitles": {})")};
    const EvalVideo video = fixtures::video();
    EXPECT_ERRC(fetch_snapshot(root().path(), video, fake.stage(), root() / "work", {}, kFetched),
                Errc::not_found)
        << "a video without captions";
    EXPECT_FALSE(std::filesystem::exists(root() / "data"));
}

TEST_F(SnapshotTest, AVideoNeverFetchedHasNone) {
    EXPECT_EQ(unwrap(read_snapshot(root().path(), fixtures::video())), std::nullopt);
}

TEST_F(SnapshotTest, ASnapshotChangedSinceItWasKeptIsRefused) {
    const EvalVideo video = fixtures::video();
    ASSERT_OK(keep(video));
    const std::string kept = root().read_file(file(video));
    (void)root().write_file(file(video), replace_all(kept, "Radium glows.", "Radium glows brightly."));
    const auto changed = read_snapshot(root().path(), video);
    EXPECT_ERRC(changed, Errc::integrity) << "a snapshot edited by hand is not the one the eval measured";
    EXPECT_THAT(changed.error().message(), HasSubstr("fetch it again"));

    (void)root().write_file(file(video), "{");
    EXPECT_ERRC(read_snapshot(root().path(), video), Errc::invalid_argument) << "not JSON";
    (void)root().write_file(file(video), R"({"captions": "Radium glows."})");
    EXPECT_ERRC(read_snapshot(root().path(), video), Errc::invalid_argument) << "not a source";

    (void)root().write_file(file(video), kept);
    const std::string record = root().read_file("data/radium-talk/snapshot.toml");
    (void)root().write_file("data/radium-talk/snapshot.toml", record + "note = \"checked\"\n");
    EXPECT_ERRC(read_snapshot(root().path(), video), Errc::invalid_argument)
        << "a record with a key it never has";
    (void)root().write_file("data/radium-talk/snapshot.toml", "fetched = \"2026-10-01T21:40:00Z\"\n");
    EXPECT_ERRC(read_snapshot(root().path(), video), Errc::invalid_argument) << "a record without its hash";
}

TEST_F(SnapshotTest, TheSetMayPinASnapshot) {
    EvalVideo video = fixtures::video();
    const Snapshot kept = unwrap(keep(video));

    video.snapshot = kept.hash;
    EXPECT_EQ(unwrap(read_snapshot(root().path(), video)), kept);
    EXPECT_EQ(unwrap(keep(video)), kept) << "fetched again, the same";

    video.snapshot = "0f0f";
    const auto other = read_snapshot(root().path(), video);
    EXPECT_ERRC(other, Errc::conflict);
    EXPECT_THAT(other.error().message(),
                HasSubstr(std::format("the set pins the snapshot 0f0f, and this one is {}", kept.hash)));
    const std::string before = root().read_file(file(video));
    EXPECT_ERRC(keep(video, fixtures::source_of("WEBVTT\n\n00:00.000 --> 00:02.000\nRadium glowed.\n")),
                Errc::conflict)
        << "YouTube's captions changed";
    EXPECT_EQ(root().read_file(file(video)), before) << "the pinned snapshot is kept";
}

TEST_F(SnapshotTest, CaptionsOfAnotherKindThanTheSetSaysAreNotKept) {
    EvalVideo video = fixtures::video();
    video.captions = media::CaptionKind::automatic;
    const auto kept = keep(video);
    EXPECT_ERRC(kept, Errc::invalid_argument);
    EXPECT_THAT(kept.error().message(),
                HasSubstr("the set says its captions are automatic, and YouTube gives manual ones"));
    EXPECT_FALSE(std::filesystem::exists(file(video)));
}

TEST_F(SnapshotTest, ACcByVideoMustShowItsLicence) {
    EvalVideo video = fixtures::video();
    video.licence = Licence::cc_by;
    const std::string vtt{kVtt};

    const auto unlicensed = keep(video, fixtures::source_of(vtt, std::nullopt));
    EXPECT_ERRC(unlicensed, Errc::invalid_argument);
    EXPECT_THAT(unlicensed.error().message(), HasSubstr("the set says CC BY, and YouTube shows no licence"));
    const auto standard = keep(video, fixtures::source_of(vtt, "Standard YouTube License"));
    EXPECT_ERRC(standard, Errc::invalid_argument);
    EXPECT_THAT(standard.error().message(), HasSubstr("YouTube shows Standard YouTube License"));
    EXPECT_FALSE(std::filesystem::exists(file(video)));

    EXPECT_OK(keep(video));
    video.licence = Licence::us_public_domain;
    EXPECT_OK(keep(video, fixtures::source_of(vtt, std::nullopt)))
        << "a federal work says nothing of a licence on YouTube";
}

TEST_F(SnapshotTest, WhatIsNotASourceIsNotKept) {
    EXPECT_ERRC(
        keep_snapshot(root().path(), fixtures::video(), json::Json{{"captions", "Radium glows."}}, kFetched),
        Errc::invalid_argument);
}

TEST_F(SnapshotTest, TheIngestReadsASnapshotAsTheSourceItWas) {
    const EvalVideo video = fixtures::video();
    const Snapshot snapshot = unwrap(keep(video));
    const library::Job job{.id = "J1", .source = file(video).string()};
    pipeline::StageContext context{.job = &job, .work_directory = root() / "work"};
    const pipeline::SourceStage stage;
    context.fingerprint = unwrap(stage.fingerprint(context));
    const pipeline::StageOutput output = unwrap(stage.run(context));
    EXPECT_EQ(json::content_hash(output.artifact).hex(), snapshot.hash)
        << "the same artifact: a snapshot runs through the stages as the video would";
}

}  // namespace
}  // namespace rr::eval

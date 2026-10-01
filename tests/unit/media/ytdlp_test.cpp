// yt-dlp is played by a shell script each test writes: what it says, what it
// writes, how it fails. The script records the arguments it was given.
#include <rr/media/ytdlp.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <rr/core/file_io.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

namespace rr::media {
namespace {

using testkit::unwrap;
using transcript::CaptionFormat;

constexpr std::string_view kUrl = "https://www.youtube.com/watch?v=abc123XYZ_0";

class FakeYtDlp {
public:
    // `body`: what the script does once it has recorded its arguments.
    explicit FakeYtDlp(std::string_view body) {
        script_ = directory_.write_file("yt-dlp", std::format("#!/bin/sh\nprintf '%s\\n' \"$@\" > '{}'\n{}\n",
                                                              (directory_ / "arguments").string(), body));
        std::filesystem::permissions(script_, std::filesystem::perms::owner_all);
    }

    [[nodiscard]] YtDlp ytdlp(std::chrono::milliseconds timeout = std::chrono::seconds{20}) const {
        return YtDlp{YtDlpOptions{.executable = script_.string(), .timeout = timeout}};
    }

    [[nodiscard]] std::vector<std::string> arguments() const {
        const Result<std::string> written = read_file(directory_ / "arguments");
        std::vector<std::string> lines;
        if (!written.has_value()) {
            return lines;
        }
        std::size_t begin = 0;
        while (begin < written->size()) {
            const std::size_t end = written->find('\n', begin);
            lines.push_back(written->substr(begin, end - begin));
            begin = end + 1;
        }
        return lines;
    }

    [[nodiscard]] const testkit::TempDir& directory() const { return directory_; }

private:
    testkit::TempDir directory_{"rr_ytdlp"};
    std::filesystem::path script_;
};

TEST(YtDlp, DescribesTheVideo) {
    const FakeYtDlp fake{R"(echo '{"id": "abc123XYZ_0", "title": "On radium", "language": "en"}')"};
    const Video video = unwrap(fake.ytdlp().video(kUrl));
    EXPECT_EQ(video.id, "abc123XYZ_0");
    EXPECT_EQ(video.title, "On radium");
    EXPECT_EQ(fake.arguments(), (std::vector<std::string>{"--dump-single-json", "--no-playlist",
                                                          "--skip-download", "--ignore-no-formats-error",
                                                          "--no-warnings", "--", std::string{kUrl}}))
        << "the address comes last, after \"--\"";
}

TEST(YtDlp, OnlyAWebAddressIsGiven) {
    const FakeYtDlp fake{"exit 0"};
    const Result<Video> video = fake.ytdlp().video("--exec=touch /tmp/owned");
    ASSERT_FALSE(video.has_value());
    EXPECT_EQ(video.error().code(), Errc::invalid_argument);
    EXPECT_TRUE(fake.arguments().empty()) << "yt-dlp never ran";
}

TEST(YtDlp, DownloadsTheCaptionsChosen) {
    const FakeYtDlp fake{R"(while [ $# -gt 0 ]; do
  if [ "$1" = "--paths" ]; then directory="$2"; fi
  shift
done
printf '%s' '{"events": []}' > "$directory/captions.en-orig.json3")"};
    const std::filesystem::path into = fake.directory() / "work";
    const Captions captions = unwrap(fake.ytdlp().captions(
        kUrl,
        CaptionChoice{.language = "en-orig", .kind = CaptionKind::automatic, .format = CaptionFormat::json3},
        into));
    EXPECT_EQ(captions.content, R"({"events": []})");
    EXPECT_EQ(captions.format, CaptionFormat::json3);
    EXPECT_EQ(fake.arguments(),
              (std::vector<std::string>{"--skip-download", "--no-playlist", "--ignore-no-formats-error",
                                        "--no-warnings", "--write-auto-subs", "--sub-langs", "en-orig",
                                        "--sub-format", "json3", "--paths", into.string(), "--output",
                                        "captions.%(ext)s", "--", std::string{kUrl}}));
}

TEST(YtDlp, CaptionsNeverWrittenAreNotFound) {
    const FakeYtDlp fake{"exit 0"};
    (void)fake.directory().write_file("work/captions.en.vtt", "WEBVTT\n");  // from an earlier try
    const Result<Captions> captions = fake.ytdlp().captions(
        kUrl, CaptionChoice{.language = "en", .kind = CaptionKind::manual, .format = CaptionFormat::vtt},
        fake.directory() / "work");
    ASSERT_FALSE(captions.has_value());
    EXPECT_EQ(captions.error().code(), Errc::not_found);
    EXPECT_EQ(captions.error().message(), "yt-dlp wrote no captions en (manual) in vtt");
    EXPECT_TRUE(std::ranges::contains(fake.arguments(), "--write-subs")) << "the creator's captions";
}

TEST(YtDlp, FailuresAreToldInRReportsTerms) {
    struct Case {
        std::string_view says;
        Errc code;
    };
    for (const Case& failure : {
             Case{
                 "ERROR: [youtube] abc: Sign in to confirm you\u2019re not a bot. Use --cookies-from-browser",
                 Errc::needs_user},
             Case{"ERROR: [youtube] abc: Sign in to confirm your age", Errc::needs_user},
             Case{"ERROR: [youtube] abc: Join this channel to get access to members-only content",
                  Errc::needs_user},
             Case{"ERROR: [youtube] abc: Private video. Sign in if you've been granted access",
                  Errc::not_found},
             Case{"ERROR: [youtube] abc: Video unavailable", Errc::not_found},
             Case{"ERROR: [youtube] abc: This video has been removed by the uploader", Errc::not_found},
             Case{"ERROR: Unable to download webpage: HTTP Error 429: Too Many Requests", Errc::rate_limited},
             Case{"ERROR: Unable to download webpage: HTTP Error 503: Service Unavailable",
                  Errc::unavailable},
             Case{"ERROR: Unable to download webpage: HTTP Error 502: Bad Gateway", Errc::unavailable},
             Case{"ERROR: [generic] Unsupported URL: https://example.com/", Errc::invalid_argument},
             Case{"ERROR: Unable to download webpage: <urlopen error [Errno -3] Temporary failure>",
                  Errc::network},
             Case{"ERROR: something new", Errc::unavailable},
         }) {
        const FakeYtDlp fake{
            std::format("echo 'WARNING: a warning' >&2\necho \"{}\" >&2\nexit 1", failure.says)};
        const Result<Video> video = fake.ytdlp().video(kUrl);
        ASSERT_FALSE(video.has_value()) << failure.says;
        EXPECT_EQ(video.error().code(), failure.code) << failure.says;
        EXPECT_NE(video.error().message().find(failure.says), std::string::npos)
            << "the message quotes what yt-dlp said: " << video.error().message();
    }
}

TEST(YtDlp, AFailureQuotesItsLastError) {
    const FakeYtDlp fake{
        "echo 'WARNING: a warning' >&2\necho 'ERROR: [youtube] abc: Video unavailable' >&2\n"
        "echo 'more detail' >&2\nexit 1"};
    EXPECT_EQ(fake.ytdlp().video(kUrl).error().message(),
              "the video is unavailable: ERROR: [youtube] abc: Video unavailable");
}

TEST(YtDlp, ItsAbsenceIsTheUsersToFix) {
    const YtDlp missing{YtDlpOptions{.executable = "/nonexistent/yt-dlp"}};
    const Result<Video> video = missing.video(kUrl);
    ASSERT_FALSE(video.has_value());
    EXPECT_EQ(video.error().code(), Errc::needs_user);
    EXPECT_EQ(video.error().message(),
              "/nonexistent/yt-dlp is not installed: python3 -m pip install -U yt-dlp");
}

TEST(YtDlp, ADescriptionThatIsNoJsonIsAFailure) {
    const FakeYtDlp fake{"echo 'not json'"};
    EXPECT_EQ(fake.ytdlp().video(kUrl).error().code(), Errc::unavailable);
}

TEST(YtDlp, ItsTimeIsLimited) {
    const FakeYtDlp fake{"sleep 10"};
    EXPECT_EQ(fake.ytdlp(std::chrono::milliseconds{200}).video(kUrl).error().code(), Errc::timeout);
}

}  // namespace
}  // namespace rr::media

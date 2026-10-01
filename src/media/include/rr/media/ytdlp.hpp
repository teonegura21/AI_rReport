// rr/media/ytdlp.hpp — yt-dlp, run safely.
//
// Why this exists
//   YouTube changes often and resists downloaders. yt-dlp keeps up with it,
//   so rReport runs yt-dlp rather than imitating it. It is the user's to
//   install and update, never bundled (plan §4); `rreport doctor` checks it.
//   This class is the only code that runs it, and it does three things:
//     * It runs it through rr_proc: no shell, a timeout, a stop. A URL always
//       comes after "--", so no address can be read as an option, and only
//       web addresses are accepted.
//     * It reads what yt-dlp writes: the video's JSON, the caption file.
//     * It says what went wrong in rReport's terms:
//       - a bot check, or an age check, is Errc::needs_user: `rreport doctor`
//         explains, and cookies are strictly opt-in;
//       - a private, removed or unavailable video is Errc::not_found;
//       - a throttle is Errc::rate_limited, the network Errc::network;
//       - yt-dlp missing is Errc::needs_user too, since the user installs it.
//
// Thread-safety: const methods; each call runs its own process.
#pragma once

#include <chrono>
#include <filesystem>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

#include <rr/core/result.hpp>
#include <rr/media/video.hpp>
#include <rr/transcript/captions.hpp>

namespace rr::media {

struct YtDlpOptions {
    std::string executable = "yt-dlp";  // a name in PATH, or a path
    std::chrono::milliseconds timeout{std::chrono::minutes{3}};
};

// A caption file, downloaded.
struct Captions {
    std::string content{};
    transcript::CaptionFormat format = transcript::CaptionFormat::json3;
};

class YtDlp {
public:
    explicit YtDlp(YtDlpOptions options = {}) : options_(std::move(options)) {}

    // The video at `url` (`yt-dlp -J`).
    [[nodiscard]] Result<Video> video(std::string_view url, const std::stop_token& stop = {}) const;

    // The captions `choice` of the video at `url`, downloaded into
    // `directory` and read back.
    [[nodiscard]] Result<Captions> captions(std::string_view url, const CaptionChoice& choice,
                                            const std::filesystem::path& directory,
                                            const std::stop_token& stop = {}) const;

private:
    YtDlpOptions options_;
};

}  // namespace rr::media

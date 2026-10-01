// rr/media/video.hpp — a video as yt-dlp describes it, and the captions to read.
//
// Why this exists
//   A job starts from a URL. yt-dlp (`yt-dlp -J`) says what is behind it: the
//   title and description the outline reads, the chapters that cut the talk
//   into sections (plan §5, stage 8), the licence the eval records, and the
//   captions on offer. Its JSON is large and loose: fields come and go, and
//   are null when unknown. This header reads what rReport uses: strictly
//   where a mistake would matter (one video, with an ID), leniently elsewhere
//   (a malformed chapter is left out, not fatal).
//
// Which captions
//   The transcript is what was said. So captions are chosen in the language
//   spoken, and never a translation:
//     1. the spoken language is the one YouTube gives the video, or else the
//        one its speech recognition heard (the track yt-dlp names
//        "<language>-orig");
//     2. the creator's captions in that language are read first, a regional
//        variant too ("en-GB" for "en");
//     3. else the speech recognition's own track ("en-orig");
//     4. when the spoken language is never told, the creator's captions are
//        read if they offer one track only, which can only be theirs.
//   Of a track's formats, json3 is read first, then WebVTT, then SubRip
//   (rr/transcript/captions.hpp). No track to read is Errc::not_found: the
//   video needs speech recognition (milestone M4).
//
// Thread-safety: plain values and pure functions.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/core/time_span.hpp>
#include <rr/json/json.hpp>
#include <rr/transcript/captions.hpp>

namespace rr::media {

enum class CaptionKind : std::uint8_t {
    manual,     // written by the creator
    automatic,  // YouTube's speech recognition
};

[[nodiscard]] std::string_view to_string(CaptionKind kind) noexcept;

// A caption track on offer.
struct CaptionOffer {
    std::string language{};  // as yt-dlp names it: "en", "en-GB", "en-orig"
    CaptionKind kind = CaptionKind::manual;
    std::vector<std::string> formats{};  // "json3", "vtt", "srv3", …

    friend bool operator==(const CaptionOffer&, const CaptionOffer&) = default;
};

struct Chapter {
    std::string title{};
    TimeSpan span;

    friend bool operator==(const Chapter&, const Chapter&) = default;
};

struct Video {
    std::string id{};   // YouTube's: "dQw4w9WgXcQ"
    std::string url{};  // its page
    std::string title{};
    std::string description{};
    std::string channel{};
    std::optional<std::string> upload_date{};  // "2009-10-25"
    std::optional<Millis> duration{};
    std::optional<std::string> language{};  // the language spoken, when YouTube says
    std::optional<std::string> license{};   // "Creative Commons Attribution license (reuse allowed)"
    std::vector<std::string> tags{};
    std::vector<Chapter> chapters{};       // in order, when the creator marked them
    std::vector<CaptionOffer> captions{};  // the creator's, then the automatic, by language
    bool live = false;                     // live or upcoming: no captions yet

    friend bool operator==(const Video&, const Video&) = default;
};

// The video yt-dlp describes in `info`. Errc::invalid_argument when `info`
// describes no single video (a playlist, a channel) or gives it no ID.
[[nodiscard]] Result<Video> video_from_ytdlp(const json::Json& info);

// The captions to download: a track, and the format to read it in.
struct CaptionChoice {
    std::string language{};
    CaptionKind kind = CaptionKind::manual;
    transcript::CaptionFormat format = transcript::CaptionFormat::json3;

    friend bool operator==(const CaptionChoice&, const CaptionChoice&) = default;
};

// The captions of `video` that are what was said (see the header comment).
// Errc::not_found when there are none, saying what is on offer.
[[nodiscard]] Result<CaptionChoice> choose_captions(const Video& video);

}  // namespace rr::media

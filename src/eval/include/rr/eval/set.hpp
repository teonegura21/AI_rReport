// rr/eval/set.hpp — the videos the evaluation measures, and how each may be
// used.
//
// Why this exists
//   rReport is measured on a fixed set of videos (ADR-0019). A set file,
//   eval/sets/<set>.toml, names each one and what the eval must know of it:
//     * its licence and attribution. Only CC BY or the US public domain:
//       the eval commits its captions, and may publish what is made of them;
//     * its type (talk, conversation, tutorial, narrative), so results can
//       be read by the kind of video;
//     * its captions, manual or automatic. The eval measures both, and a
//       video whose captions turn out to be the other kind is a mistake to
//       stop at, not a result to average in;
//     * its split: dev, for every iteration, or test, kept for gates and
//       milestones;
//     * optionally, its snapshot's hash: the captions as they were read and
//       checked, pinned (rr/eval/snapshot.hpp).
//   Reading a set is strict: an unknown key, a missing one, a value out of
//   its choices, or two videos with one id is an error that names it.
//
// Thread-safety: plain values and pure functions.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/media/video.hpp>

namespace rr::eval {

enum class VideoType : std::uint8_t {
    talk,          // a talk or a lecture: one speaker, prepared
    conversation,  // a podcast, an interview, a panel
    tutorial,      // what is shown matters: slides, code, a whiteboard
    narrative,     // a documentary, a news report
};

enum class Split : std::uint8_t {
    dev,   // measured at every iteration
    test,  // kept for gates and milestones (ADR-0019)
};

enum class Licence : std::uint8_t {
    cc_by,             // Creative Commons Attribution
    us_public_domain,  // a work of the US federal government (17 U.S.C. §105)
};

[[nodiscard]] std::string_view to_string(VideoType type) noexcept;
[[nodiscard]] std::string_view to_string(Split split) noexcept;
[[nodiscard]] std::string_view to_string(Licence licence) noexcept;

struct EvalVideo {
    std::string id{};  // the set's name for it, and its directory's: [a-z0-9-], up to 64
    std::string title{};
    std::string url{};
    VideoType type = VideoType::talk;
    media::CaptionKind captions = media::CaptionKind::manual;
    Split split = Split::dev;
    Licence licence = Licence::us_public_domain;
    std::string attribution{};              // who to credit, as the licence asks
    std::optional<std::string> snapshot{};  // the pinned snapshot's hash

    friend bool operator==(const EvalVideo&, const EvalVideo&) = default;
};

struct EvalSet {
    std::string name{};
    std::vector<EvalVideo> videos{};  // in the file's order

    // The video `id`, or nullptr.
    [[nodiscard]] const EvalVideo* find(std::string_view id) const noexcept;

    friend bool operator==(const EvalSet&, const EvalSet&) = default;
};

// Whether `id` can name a video: 1 to 64 of [a-z0-9-], starting with a
// letter or a digit. It names a directory, so nothing else is allowed.
[[nodiscard]] bool valid_video_id(std::string_view id) noexcept;

// The set `name` from its TOML text (see the header comment).
// Errc::invalid_argument naming what is wrong, and where.
[[nodiscard]] Result<EvalSet> parse_set(std::string_view name, std::string_view toml);

// eval_root/sets/<name>.toml. Errc::not_found when there is none.
[[nodiscard]] Result<EvalSet> read_set(const std::filesystem::path& eval_root, std::string_view name);

}  // namespace rr::eval

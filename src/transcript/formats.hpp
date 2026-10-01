// formats.hpp — each caption format, cut into cues (private to rr_transcript).
//
// Each reader knows its format's syntax and nothing else: cues.hpp reads the
// cues' texts into words the same way for all of them. Errors name the line,
// or the json3 event, where the file stops being its format.
#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include <rr/core/result.hpp>

#include "cues.hpp"

namespace rr::transcript::formats {

// The lines of `content`, without their ends ("\n", "\r\n" or "\r"), and
// without a byte order mark before the first.
[[nodiscard]] std::vector<std::string_view> lines_of(std::string_view content);

// The latest time a caption may give: about 11.5 days. No video is longer,
// and a time far beyond it could overflow the arithmetic of reading cues.
inline constexpr Millis kLatest{1'000'000'000};

// A caption timing: "01:02:03.456" or "02:03.456", with a comma for the point
// as SRT writes it ("01:02:03,456"); nullopt for anything else, or a time
// after kLatest.
[[nodiscard]] std::optional<Millis> timing(std::string_view text);

[[nodiscard]] Result<std::vector<cues::Cue>> json3(std::string_view content);
[[nodiscard]] Result<std::vector<cues::Cue>> vtt(std::string_view content);
[[nodiscard]] Result<std::vector<cues::Cue>> srt(std::string_view content);

// Plain text has no syntax to break: every paragraph is a cue, at 0.
[[nodiscard]] std::vector<cues::Cue> text(std::string_view content);

}  // namespace rr::transcript::formats

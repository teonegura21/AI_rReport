// rr/transcript/captions.hpp — captions, read into the words said and when.
//
// Why this exists
//   Captions are the backbone of a transcript (plan §3.2): the creator's
//   when there are any, YouTube's speech recognition otherwise. They come in
//   several formats, cut into cues that break sentences anywhere, with markup,
//   sound descriptions and, in automatic captions, the time of every word.
//   Read once into one form, timed words, everything after them (punctuation,
//   sentences) is written once.
//
// The formats
//   * json3, YouTube's timed text, which rReport downloads: events of
//     segments. In automatic captions a segment is a word with its own start.
//   * WebVTT: cues with tags (<i>, <c.color>, <v Ada>). In YouTube's
//     automatic captions a timestamp precedes each word, and cues roll: each
//     repeats the line before it, which is read once.
//   * SubRip (SRT): numbered cues with HTML-like tags.
//   * Plain text: words without times.
//   A cue whose timing cannot be read is an error naming its line: a file
//   read wrongly would put words at the wrong seconds of the video.
//
// What is kept
//   * What was said, as words: whitespace-free tokens as written ("Hello,",
//     "37%"), in NFC, with invalid bytes as U+FFFD. Markup is removed and
//     character references are decoded ("&amp;" is "&").
//   * When: each word's start and end. Without word times, a cue's time is
//     shared among its words by their length. Starts never decrease: cues out
//     of order are sorted, and a word starting before the one before it is
//     moved to that one's start.
//   * Who: a WebVTT voice (<v Ada>) names a speaker, and so does a
//     captioner's label at the start of a line, in capitals ("ADA LOVELACE:
//     ..."). ">>" and a dialogue dash ("- Hi.") mark a new speaker without
//     naming them. Speakers are never guessed.
//   * Paragraphs: a new speaker starts one, and so does a blank line in plain
//     text. A sentence never runs across a paragraph.
//
// What is set apart
//   Sound descriptions are not speech: "[Music]", "(laughs)", the music notes
//   around lyrics. They are kept apart, as sounds, never mixed into what was
//   said. A word the captioner missed ("[inaudible]", "[unclear]") stays as
//   the word "[unclear]": a gap in what was heard is not hidden.
//
// Thread-safety: pure functions.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/core/time_span.hpp>

namespace rr::transcript {

enum class CaptionFormat : std::uint8_t {
    json3,
    vtt,
    srt,
    text,
};

[[nodiscard]] std::string_view to_string(CaptionFormat format) noexcept;

// The format of a caption file by its name: ".json3", ".vtt", ".srt" or
// ".txt", in any case; nullopt for any other.
[[nodiscard]] std::optional<CaptionFormat> format_of(std::string_view file_name);

struct Word {
    std::string text{};  // whitespace-free, as written
    Millis start{0};
    Millis end{0};                         // never before start
    std::optional<std::string> speaker{};  // when the captions name one
    bool paragraph = false;                // a paragraph starts with this word

    friend bool operator==(const Word&, const Word&) = default;
};

struct Sound {
    std::string text{};  // as written: "[Music]", "(laughs)"
    Millis start{0};
    Millis end{0};

    friend bool operator==(const Sound&, const Sound&) = default;
};

struct CaptionTrack {
    std::vector<Word> words{};    // in the order said
    std::vector<Sound> sounds{};  // in the order heard
    bool timed = true;            // false for plain text, whose times are all 0

    friend bool operator==(const CaptionTrack&, const CaptionTrack&) = default;
};

// The words of `content`, read as `format` (see the header comment).
// Errc::invalid_argument when it is not that format: the message says where.
[[nodiscard]] Result<CaptionTrack> read_captions(std::string_view content, CaptionFormat format);

}  // namespace rr::transcript

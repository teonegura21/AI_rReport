// cues.hpp — what every caption format shares (private to rr_transcript).
//
// A format's reader cuts its file into cues: when the cue is shown, its text
// with the markup removed, the times the format gives inside it, and the
// voices that speak it. add_cues() reads the cues' texts into words and
// sounds the same way for every format: the rules of
// rr/transcript/captions.hpp, "What is kept" and "What is set apart".
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/time_span.hpp>
#include <rr/transcript/captions.hpp>

namespace rr::transcript::cues {

// A time the format gives at a point of a cue's text: a word's start.
struct Mark {
    std::size_t offset = 0;  // in Cue::text
    Millis time{0};
};

// The run of a cue's text a WebVTT voice speaks: [begin, end).
struct Voice {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::string name{};
};

struct Cue {
    Millis start{0};
    Millis end{0};
    std::string text{};           // markup removed, references decoded; lines split by '\n'
    std::vector<Mark> marks{};    // in text order
    std::vector<Voice> voices{};  // in text order, not overlapping
    bool paragraph = false;       // starts a paragraph (a blank line in plain text)
};

// `text` with its character references decoded: "&amp;" is "&", "&#39;" and
// "&#x27;" are "'". A reference rReport does not know stays as written.
[[nodiscard]] std::string decode_references(std::string_view text);

// The words and sounds of `cues`, read in the order they start, added to
// `track` (see rr/transcript/captions.hpp).
void add_cues(std::vector<Cue> cues, CaptionTrack& track);

}  // namespace rr::transcript::cues

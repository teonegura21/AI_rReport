// rr/transcript/projection.hpp — a model's punctuation, laid onto the words said.
//
// Why this exists
//   Automatic captions often come without punctuation, in lowercase: "so
//   marie curie won two nobel prizes she called radium". Sentences cannot be
//   found in that, and names cannot be told from words (rr/text/entities.hpp
//   finds names by their capitals). A model punctuates it well, but a model
//   also rewrites: it smooths grammar, drops a repetition, turns "thirty
//   seven" into "37". A transcript is what was said, so the model's text is
//   never taken. Its punctuation and capitals are laid onto the captions'
//   words instead, and so it can never change one (plan §3.2).
//
// The rules
//   * The words of the captions and of the model's text are aligned, folded
//     (rr/text/unicode.hpp), with the fewest changes. Where the model changed,
//     added or dropped a word, the change is counted, never taken.
//   * Every character of the captions stays, in order. Two things change:
//       - a word the model wrote in other capitals takes them: "marie" is
//         "Marie" where the model wrote "Marie". Only the capitals: "resume"
//         stays "resume" where the model wrote "résumé";
//       - punctuation is added around the captions' words: what the model wrote
//         after a word (".", ",", "?", "”", ")") goes after it, what it wrote
//         before one ("“", "(") goes before. A word the captions already
//         punctuate keeps their punctuation.
//   * Only punctuation that changes no word and no number is added: the
//     sentence ends of every script (". ! ? …", "。", "؟", "।"), commas,
//     semicolons, colons, dashes, quotes and brackets. Never a hyphen, which
//     joins words, nor a sign or a percent, which change numbers, nor any
//     symbol.
//   * A word the model added is not taken; a sentence end written after it
//     still ends the sentence, after the caption word before it.
//   So the words of a projection are the captions' words, one for one, and
//   folded they are equal: the projection checks this before it returns.
//
// Thread-safety: pure functions.
#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/transcript/captions.hpp>

namespace rr::transcript {

// The most words of each text one projection aligns. The alignment takes
// time and memory in proportion to their product, and a model writes a
// window of a few hundred words at a time.
inline constexpr std::size_t kMaxProjected = 2048;

struct Projection {
    std::vector<Word> words{};  // the captions' words, punctuated, with their times
    std::size_t matched = 0;    // words the model wrote as the captions have them
    std::size_t changed = 0;    // words the model wrote otherwise: not taken
    std::size_t added = 0;      // words the model added: not taken
    std::size_t dropped = 0;    // words the model left out: kept, unpunctuated

    friend bool operator==(const Projection&, const Projection&) = default;
};

// `words` punctuated as the model punctuated them in `punctuated` (see the
// header comment). Errc::invalid_argument when either holds more than
// kMaxProjected words (rr/text/tokens.hpp): project a window at a time.
[[nodiscard]] Result<Projection> project(const std::vector<Word>& words, std::string_view punctuated);

}  // namespace rr::transcript

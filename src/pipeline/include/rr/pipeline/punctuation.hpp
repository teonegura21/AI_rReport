// rr/pipeline/punctuation.hpp — automatic captions, punctuated by a model a
// window at a time, without a word changed.
//
// Why this exists
//   YouTube's speech recognition writes what was said without punctuation,
//   mostly in lowercase. Sentences cannot be found in that, nor names told
//   from words, and every stage after the transcript works by sentence. A
//   model punctuates it well, and the projection (rr/transcript/projection.hpp)
//   takes only its punctuation and capitals, never its words. This header
//   cuts the words into the windows the model is sent, and checks each
//   answer before it is taken.
//
// Windows
//   A whole video does not fit one call, and a model punctuates a page
//   better than a book. So the words are cut into windows of about
//   `target` bytes (UTF-8 bytes stand in for tokens: about four a token in
//   English, three in Chinese). Each cut falls where the speaker paused
//   longest, among the cuts that leave between `least` and `most` bytes,
//   since a pause is where a sentence most likely ends. Ties go to the cut
//   nearest `target`.
//   A window cut mid-sentence would be punctuated as if it began or ended
//   there: a capital after the cut, a full stop before it. So each window is
//   sent with its context, up to `context` bytes of the words before it and
//   after it. The model punctuates all of it, and only the window's own
//   words are kept. Every word is in exactly one window, and every word at a
//   window's edge was punctuated with the words around it in view.
//
// The checks an answer must pass (sent back to the model when it fails)
//   * Its words are the excerpt's words: at most one in twenty, and at least
//     two, may be changed, added or left out. Those are never taken, but a
//     model that rewrites has not just punctuated, and its punctuation is
//     doubtful.
//   * It is punctuated, as rr/transcript/sentences.hpp's punctuated() sees
//     it: the very test that sent the captions to be punctuated.
//
// Thread-safety: pure functions.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/transcript/captions.hpp>

namespace rr::pipeline {

// In bytes of the words joined by single spaces.
struct WindowSize {
    std::size_t least = 600;
    std::size_t target = 1000;
    std::size_t most = 1400;
    std::size_t context = 200;  // before the window's words, and after them
};

// A window of the words: its own words, [begin, end), and with its context,
// [context_begin, context_end).
struct Window {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::size_t context_begin = 0;
    std::size_t context_end = 0;

    friend bool operator==(const Window&, const Window&) = default;
};

// The windows of `words`, in order (see the header comment). A word longer
// than `size.most` is a window of its own. Requires least <= target <= most.
[[nodiscard]] std::vector<Window> windows_of(const std::vector<transcript::Word>& words,
                                             const WindowSize& size = {});

// What the model is sent of `window`: its words with their context, joined by
// single spaces.
[[nodiscard]] std::string excerpt_of(const std::vector<transcript::Word>& words, const Window& window);

// What is wrong with `answer` as `window`'s excerpt punctuated (see the header
// comment), each said as what to do instead; empty when nothing is.
[[nodiscard]] std::vector<std::string> punctuation_problems(const std::vector<transcript::Word>& words,
                                                            const Window& window, std::string_view answer);

// `window`'s own words, punctuated as `answer` punctuates them, with their
// times. Errc::invalid_argument when the answer cannot be projected.
[[nodiscard]] Result<std::vector<transcript::Word>> punctuated_words(
    const std::vector<transcript::Word>& words, const Window& window, std::string_view answer);

}  // namespace rr::pipeline

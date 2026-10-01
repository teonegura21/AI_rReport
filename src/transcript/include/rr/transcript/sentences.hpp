// rr/transcript/sentences.hpp — timed words, cut into the transcript's sentences.
//
// Why this exists
//   The transcript is the closed world everything answers to, numbered
//   sentence by sentence, S1…Sn, each with when it was said
//   (rr/knowledge/transcript.hpp). Captions cut sentences anywhere. Here the
//   words are joined again and cut where sentences end (rr/text/sentences.hpp).
//
// The rules
//   * A sentence never runs across a paragraph: a new speaker starts one.
//   * A sentence is said from its first word's start to its last word's end.
//     One that starts or ends inside a word, as in Chinese, which needs no
//     spaces, takes that word's time in proportion.
//   * A sentence of more than `max_words` words is cut where its words pause
//     longest, until no part is longer. Ties go to the cut nearest the
//     middle. Unpunctuated captions give such sentences: the cuts bound them
//     until a model punctuates them (rr/transcript/projection.hpp).
//   * A sentence's speaker is its first word's, when the captions name one.
//
// Thread-safety: pure functions.
#pragma once

#include <cstddef>
#include <vector>

#include <rr/knowledge/transcript.hpp>
#include <rr/transcript/captions.hpp>

namespace rr::transcript {

struct SentenceOptions {
    std::size_t max_words = 60;
};

// The transcript of `words`, its sentences numbered from S1 (see the header
// comment).
[[nodiscard]] knowledge::Transcript transcript_of(const std::vector<Word>& words,
                                                  const SentenceOptions& options = {});

// Whether `words` are punctuated: they end a sentence at least once every
// `every` words, on average. Automatic captions often are not, and go through
// project() first. Too few words to tell are punctuated when one ends a
// sentence.
[[nodiscard]] bool punctuated(const std::vector<Word>& words, std::size_t every = 40);

}  // namespace rr::transcript

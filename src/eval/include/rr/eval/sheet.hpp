// rr/eval/sheet.hpp — the sheet a person reviews a gold file with.
//
// Why this exists
//   Correcting silver into gold is reading: each claim against the words
//   that say it, the words against the video. The review sheet puts them
//   side by side, in Markdown:
//     * each fact, with its stretch as a link to that moment of the video,
//       how many drafts found it, its importance, its claim and its quote;
//     * whether the quote is said there (gold_problems());
//     * a fact only one draft found is marked: it is the first to doubt;
//     * gold drafted on another snapshot than the one kept now: YouTube's
//       captions changed, and every anchor must be checked again.
//   A person keeps the sheet open beside eval/gold/<id>.toml, and runs it
//   again after correcting, until nothing is left to correct.
//
// Checked without a model
//   A quote is found in the transcript without its punctuation
//   (rr/text/align.hpp folds it away), so the sheet checks against the
//   snapshot's captions cut into sentences as they are (caption_transcript()):
//   reviewing never needs the pod.
//
// Thread-safety: pure functions.
#pragma once

#include <string>

#include <rr/core/result.hpp>
#include <rr/eval/gold.hpp>
#include <rr/eval/set.hpp>
#include <rr/knowledge/transcript.hpp>
#include <rr/pipeline/source.hpp>

namespace rr::eval {

// The transcript of `source`'s captions, cut into sentences as they are,
// without a model: what anchors are checked against. Its sentence numbers
// are its own: gold never names them.
[[nodiscard]] Result<knowledge::Transcript> caption_transcript(const pipeline::Source& source);

// The address of the moment `at` of the video at `url`.
[[nodiscard]] std::string at_time(std::string_view url, Millis at);

// The review sheet of `gold`, a gold file of `video`, checked against
// `transcript`: the captions of the video's snapshot, whose hash is
// `snapshot` (see the header comment).
[[nodiscard]] std::string review_sheet(const Gold& gold, const EvalVideo& video,
                                       const knowledge::Transcript& transcript, std::string_view snapshot);

}  // namespace rr::eval

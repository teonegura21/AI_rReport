// rr/pipeline/outline.hpp — how a video's sections are made and joined: from
// the creator's chapters, from pauses, from several outlines agreed on; and
// how the briefs of its sections become the video's.
//
// Why this exists
//   The outline stage (rr/pipeline/outline_stage.hpp) decides where sections
//   are, and asks models only for judgment. Everything else is rules, here,
//   pure and tested on their own:
//     * The creator's chapters are times; sections are sentences. A chapter
//       starts at the sentence that starts closest to its time, so a speaker
//       a moment early or late is still the chapter's first sentence.
//     * No section runs longer than eight minutes: what a writer drafts in
//       one piece (plan §5, stage 8). A longer one is cut where the speaker
//       paused longest, among the cuts that leave each part a quarter of it
//       at least, so that it is cut in balanced parts, not trimmed; between
//       pauses as long, and when no cut leaves a quarter, nearest its middle.
//     * Without chapters or a model, the video is cut that way from the start.
//     * Several outlines agree where enough of them start a section within a
//       sentence or two of each other (rr/reason/boundaries.hpp). The title
//       of an agreed section is that of the first outline that starts a
//       section exactly there.
//     * Briefs join: the genre most of them name; their speakers and their
//       words, each once by its folded form, a role or a meaning filling an
//       empty one and never replacing one. Of more than a brief holds, those
//       that most of them name: a word every section uses matters more than
//       one a section mentions, and it is what a model listing too much
//       lists least often.
//
// Thread-safety: pure functions.
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include <rr/core/time_span.hpp>
#include <rr/knowledge/outline.hpp>
#include <rr/knowledge/transcript.hpp>
#include <rr/media/video.hpp>

namespace rr::pipeline {

// The longest a section may run.
inline constexpr Millis kLongestSection{8 * 60 * 1'000};

// The most speakers and words a joined brief keeps: about 1,500 tokens with
// their roles and meanings, the brief's share of every later prompt.
inline constexpr std::size_t kMostSpeakers = 12;
inline constexpr std::size_t kMostTerms = 40;

// Where a section starts, as a stage makes it: the index of its first
// sentence; it runs to the sentence before the next cut's.
struct Cut {
    std::size_t first = 0;
    std::string title{};

    friend bool operator==(const Cut&, const Cut&) = default;
};

// How long the section that `cuts[index]` starts runs: from its first
// sentence's start to its last's end.
[[nodiscard]] Millis section_length(std::span<const Cut> cuts, std::size_t index,
                                    const knowledge::Transcript& transcript);

// The cuts the creator's `chapters` make of `transcript`: each chapter from
// the sentence that starts closest to its time (the earlier of two as
// close), the first from the first sentence. A chapter that would start
// where the one before does, or before it, is dropped. None for a
// transcript without sentences.
[[nodiscard]] std::vector<Cut> chapter_cuts(std::span<const media::Chapter> chapters,
                                            const knowledge::Transcript& transcript);

// `cuts` with every section longer than `longest` cut again where the
// speaker paused longest (see the header comment), until none is or a part
// is one sentence. The parts of a section keep its title, numbered
// ("Radium (2 of 3)").
[[nodiscard]] std::vector<Cut> cut_long(std::vector<Cut> cuts, const knowledge::Transcript& transcript,
                                        Millis longest = kLongestSection);

// The cuts of the sentences [first, end) of `transcript` where no chapter or
// model says where: cut where the speaker paused longest until no section is
// longer than `longest`, each titled by its stretch ("12:30–19:45"). None
// for no sentences.
[[nodiscard]] std::vector<Cut> pause_cuts(const knowledge::Transcript& transcript, std::size_t first,
                                          std::size_t end, Millis longest = kLongestSection);

// Where `outlines` agree (see the header comment): the cuts that at least
// `quorum` of them place within `tolerance` sentences of each other, each
// at the position closest to them all, titled by the first outline that
// starts a section exactly there.
[[nodiscard]] std::vector<Cut> agreed_cuts(std::span<const std::vector<Cut>> outlines, int tolerance,
                                           double quorum);

// `briefs` joined (see the header comment): at most kMostSpeakers speakers
// and kMostTerms words, those the most briefs name (the first named on a
// tie), in the order first named. Genre::other for no briefs. The gist is
// left to the caller.
[[nodiscard]] knowledge::Brief join_briefs(std::span<const knowledge::Brief> briefs);

// The outline of `transcript` that `cuts` make, which start at its first
// sentence and are in order; its sections without gists.
[[nodiscard]] knowledge::Outline outline_of(std::span<const Cut> cuts,
                                            const knowledge::Transcript& transcript, knowledge::Brief brief,
                                            knowledge::SectionsFrom from);

}  // namespace rr::pipeline

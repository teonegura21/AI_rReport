// rr/reason/boundaries.hpp — where several outlines agree that a section
// starts.
//
// Why this exists
//   At the `max` tier the commander outlines a video three times (ADR-0018).
//   Outlines seldom start a section on exactly the same sentence, but they
//   mostly agree within a sentence or two: one starts the new topic at the
//   speaker's "So,", another at the sentence after. Voting on exact positions
//   would throw that agreement away, and keeping every boundary would cut
//   the video wherever any one outline did. boundary_consensus() keeps the
//   boundaries that enough outlines place near each other, each at the
//   position closest to all of them.
//
//   How: every proposed position is a candidate. A candidate's support is
//   the number of outlines with a boundary within `tolerance` sentences of
//   it; its spread, how far those boundaries are from it in all. Candidates
//   are taken best first (most support, then least spread, then the earliest).
//   One is kept when enough outlines support it and no boundary already kept
//   lies within `tolerance` of it. So each place where outlines agree gives
//   one boundary, at its most central position.
//
// Thread-safety: a pure function.
#pragma once

#include <span>
#include <vector>

namespace rr::reason {

// The boundaries that at least `quorum` (a share in (0, 1]) of `outlines`
// place within `tolerance` sentences of each other, in order. Each outline
// lists the sentence indices where its sections start, in any order.
[[nodiscard]] std::vector<int> boundary_consensus(std::span<const std::vector<int>> outlines, int tolerance,
                                                  double quorum);

}  // namespace rr::reason

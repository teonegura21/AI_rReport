// rr/reason/vote.hpp — many samples, one decision: votes per item, more
// samples only where they disagree, and the text that best stands for the
// winners.
//
// Why this exists
//   Asking a model several times and keeping what most answers say
//   (self-consistency: Wang et al., 2023) beats asking once. At equal compute
//   it also beats asking a model to verify its answer (Singhi et al., 2025).
//   Spending samples only where they disagree, instead of the same number
//   everywhere, is more than four times as efficient (Snell et al., 2024).
//
//   A mission's answer covers many items: the 25 sentences of a window, say.
//   Its samples agree on most of them. So:
//     * votes are counted per item (vote_keyed());
//     * another wave of samples is drawn only while some item is unsettled
//       and could still settle (next_wave());
//     * an item still contested at the end is escalated (to the commander,
//       at the `max` tier), never guessed.
//
//   An item is settled when its leading answer holds at least `quorum` of
//   the votes cast on it, and strictly more than any other answer. A tie
//   is never settled: the order the samples came in must not decide.
//
// Thread-safety: pure functions and plain values.
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/result.hpp>

namespace rr::reason {

// How many samples a decision gets: `first` at once; then `step` more at a
// time, while some item is unsettled and could still settle; `max` in all.
struct Waves {
    int first = 1;
    int step = 0;
    int max = 1;
    // The share of an item's votes its leading answer needs. Two thirds is
    // 0.66: 0.67 is more than two votes of three.
    double quorum = 0.5;

    friend bool operator==(const Waves&, const Waves&) = default;
};

// What is wrong with `waves` (empty: nothing): first ≥ 1, step ≥ 0,
// max ≥ first, max = first when step is 0, and a quorum in (0, 1].
[[nodiscard]] std::vector<std::string> waves_problems(const Waves& waves);

// One sample's answer for one item.
struct Ballot {
    std::string item{};   // what is voted on, e.g. "S17"
    std::string value{};  // the categorical answer, in a canonical form (e.g. canonical JSON)
    std::string text{};   // free text that comes with it, for medoid(); may be empty
    int sample = 0;
};

struct Count {
    std::string value{};
    int votes = 0;

    friend bool operator==(const Count&, const Count&) = default;
};

struct Consensus {
    std::string item{};
    // Every value voted for: most votes first, ties in the order first voted.
    std::vector<Count> counts{};
    int votes = 0;                        // ballots cast on the item
    std::optional<std::string> winner{};  // the settled answer; none: contested
    // The sample whose text best stands for the winner's ballots (medoid()).
    std::optional<int> medoid{};
};

// Whether an item with these counts (most votes first) is settled.
[[nodiscard]] bool settled(std::span<const Count> counts, double quorum) noexcept;

// The votes on each of `items`, in that order; an item no ballot is cast on
// is contested. Errc::invalid_argument for a ballot on an item not listed, or
// a second ballot of one sample on one item.
[[nodiscard]] Result<std::vector<Consensus>> vote_keyed(std::span<const std::string> items,
                                                        std::span<const Ballot> ballots, double quorum);

// How many samples the next wave draws, `drawn` having been drawn: none (0)
// when every item is settled, when `max` is reached, or when no unsettled item
// could settle even if every sample still to come agreed with its leader.
[[nodiscard]] int next_wave(std::span<const Consensus> items, int drawn, const Waves& waves) noexcept;

using Distance = std::function<double(std::string_view, std::string_view)>;

// 1 − |A ∩ B| / |A ∪ B| over the two texts' sets of words, lowercased: 0 for
// the same words, 1 for none in common (and 0 for two empty texts). A word
// is a run of letters and digits; bytes of UTF-8 sequences count as letters.
[[nodiscard]] double word_distance(std::string_view lhs, std::string_view rhs);

// The text closest to all the others: the least summed distance; a tie goes
// to the first. A real text that most answers resemble, where averaging
// answers would invent a new one. None for no text.
[[nodiscard]] std::optional<std::size_t> medoid(std::span<const std::string> texts,
                                                const Distance& distance = word_distance);

}  // namespace rr::reason

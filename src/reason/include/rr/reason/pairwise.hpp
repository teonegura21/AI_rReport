// rr/reason/pairwise.hpp — a judge's preference between two answers, asked
// in both orders.
//
// Why this exists
//   Model judges are biased:
//     * by position: they favour the answer shown first, or second;
//     * by length: they favour the longer answer;
//     * by family: they favour answers from models like themselves.
//   (Wang et al., 2023; Zheng et al., 2023; Verga et al., 2024.)
//
//   rReport counts a preference only if it survives a swap. The judge sees
//   (a, b), then (b, a), and a wins only if both orders prefer it. Anything
//   else is a tie: position bias becomes a tie instead of a win. The tally
//   also counts the pairs whose orders disagreed: a judge's measured
//   position bias, which the eval's calibration reports (ADR-0019).
//
//   Length and family biases are handled where the judging is set up. Judges
//   are of another family than the writer, and drafts are compared at
//   similar lengths.
//
// Thread-safety: pure functions and plain values.
#pragma once

#include <cstdint>
#include <string_view>

namespace rr::reason {

// What a judge said about the pair as it was shown.
enum class Preference : std::uint8_t {
    first,
    second,
    tie
};

enum class Winner : std::uint8_t {
    a,
    b,
    tie
};

[[nodiscard]] std::string_view to_string(Winner winner) noexcept;

// The judge was shown (a, b) and said `shown_a_first`, then (b, a) and said
// `shown_b_first`. A side wins only when both orders prefer it.
[[nodiscard]] Winner swapped(Preference shown_a_first, Preference shown_b_first) noexcept;

// Many pairs judged (by several judges, or of several drafts).
struct PairwiseTally {
    int a = 0;
    int b = 0;
    int ties = 0;
    int inconsistent = 0;  // pairs whose two orders disagreed

    // Adds one pair, judged in both orders.
    void add(Preference shown_a_first, Preference shown_b_first) noexcept;

    [[nodiscard]] int pairs() const noexcept { return a + b + ties; }
    // a's share, a tie counting half; 0.5 for no pair.
    [[nodiscard]] double score_a() const noexcept;
};

}  // namespace rr::reason

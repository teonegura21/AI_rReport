// rr/eval/metrics.hpp — every metric the eval scores: which scorer family
// computes it, and which way it is better.
//
// Why this exists
//   A comparison says "better" or "worse" (rr/eval/stats.hpp), and a gate
//   refuses a candidate worse than its margin (rr/eval/gate.hpp): both must
//   know, of each metric, which way is better. A gate file must name only
//   metrics this rreport scores, and that are better one way: a misspelt
//   name gated would gate nothing, silently. And an invariant asks whether
//   an entry was scored by the metric's family at all: an entry the code
//   never scored keeps no invariant, while one it scored without a share
//   (an article with no number to keep) keeps it with nothing to break.
//   So one table says it of every metric, in the order a comparison lists
//   them: what a faithful article never breaks, what it tells, how
//   faithfully, its form, the judges' agreement, and what it cost. A test
//   checks that every metric the scorers compute is in it, and only those.
//   What may be gated follows: a metric better one way, and not a cost.
//
// Thread-safety: constant data.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace rr::eval {

// Which way a metric is better.
enum class Better : std::uint8_t {
    higher,  // a share accounted for, kept, told, cited or supported
    lower,   // invalid markers, a judged issue's share, minutes, calls, tokens, escalations
    neither  // shown, never better or worse: the length ratio, the judges' agreement
};

[[nodiscard]] std::string_view to_string(Better better) noexcept;

struct Metric {
    std::string_view name{};
    std::string_view family{};  // the scorer family computing it: "code" (rr/eval/score.hpp) or
                                // "judges" (rr/eval/judgement.hpp)
    Better better = Better::neither;
    bool cost = false;  // what an article cost: reported, never gated, as quality comes first (ADR-0019)

    friend bool operator==(const Metric&, const Metric&) = default;
};

// Every metric, in the order a comparison lists them.
[[nodiscard]] std::span<const Metric> metrics() noexcept;

// The metric `name`; none for one this rreport does not score.
[[nodiscard]] const Metric* find_metric(std::string_view name) noexcept;

}  // namespace rr::eval

// rr/reason/select.hpp — the best of several candidates, by criteria in
// order of importance.
//
// Why this exists
//   At the `max` tier the writer drafts a section four times, and one draft
//   is kept (ADR-0018). Its criteria do not trade against each other. A
//   draft that drops a must-quote is worse than any draft that keeps it,
//   however well it reads. So candidates are compared lexicographically:
//     1. deterministic violations first (the Fidelity Ledger's checks);
//     2. then the issues judges found;
//     3. then how well the draft reads;
//     4. only then by order.
//   A weighted sum would let fluency buy back a fidelity failure, and its
//   weights would be one more thing to tune.
//
// Thread-safety: a pure function.
#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include <rr/core/result.hpp>

namespace rr::reason {

// The index of the best candidate: the least first key; among those, the
// least second key; and so on. A tie on every key goes to the first
// candidate. Smaller is better: negate a score to prefer it high. A key that
// is not a number is worse than any number. Errc::invalid_argument for no
// candidate, or candidates with different numbers of keys.
[[nodiscard]] Result<std::size_t> select_lexicographic(std::span<const std::vector<double>> candidates);

}  // namespace rr::reason

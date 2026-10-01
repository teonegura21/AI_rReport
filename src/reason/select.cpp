#include <rr/reason/select.hpp>

#include <cmath>
#include <format>

namespace rr::reason {

namespace {

// Whether key `lhs` is better than key `rhs`. NaN is worse than any number,
// and as bad as another NaN.
bool better(double lhs, double rhs) noexcept {
    if (std::isnan(lhs)) {
        return false;
    }
    return std::isnan(rhs) || lhs < rhs;
}

}  // namespace

Result<std::size_t> select_lexicographic(std::span<const std::vector<double>> candidates) {
    if (candidates.empty()) {
        return fail(Errc::invalid_argument, "there is no candidate to select");
    }
    const std::size_t keys = candidates.front().size();
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (candidates[index].size() != keys) {
            return fail(Errc::invalid_argument, std::format("candidate {} has {} keys, candidate 0 has {}",
                                                            index, candidates[index].size(), keys));
        }
    }
    std::size_t best = 0;
    for (std::size_t index = 1; index < candidates.size(); ++index) {
        for (std::size_t key = 0; key < keys; ++key) {
            const double challenger = candidates[index][key];
            const double holder = candidates[best][key];
            if (better(challenger, holder)) {
                best = index;
                break;
            }
            if (better(holder, challenger)) {
                break;  // the holder stays
            }
        }
    }
    return best;
}

}  // namespace rr::reason

#include <rr/reason/boundaries.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <set>
#include <tuple>

namespace rr::reason {

namespace {

struct Candidate {
    int position = 0;
    int support = 0;  // outlines with a boundary within the tolerance
    int spread = 0;   // the distances from those boundaries, summed
};

// The distance from `position` to the outline's nearest boundary.
int nearest(const std::set<int>& outline, int position) {
    int distance = std::numeric_limits<int>::max();
    const auto after = outline.lower_bound(position);
    if (after != outline.end()) {
        distance = *after - position;
    }
    if (after != outline.begin()) {
        distance = std::min(distance, position - *std::prev(after));
    }
    return distance;
}

}  // namespace

std::vector<int> boundary_consensus(std::span<const std::vector<int>> outlines, int tolerance,
                                    double quorum) {
    std::vector<std::set<int>> sets;
    sets.reserve(outlines.size());
    std::set<int> positions;
    for (const std::vector<int>& outline : outlines) {
        sets.emplace_back(outline.begin(), outline.end());
        positions.insert(outline.begin(), outline.end());
    }
    // Slack for floating point: a quorum of 0.6 of five outlines is three.
    const auto needed = static_cast<int>(std::ceil((quorum * static_cast<double>(outlines.size())) - 1e-9));

    std::vector<Candidate> candidates;
    candidates.reserve(positions.size());
    for (const int position : positions) {
        Candidate candidate{.position = position};
        for (const std::set<int>& outline : sets) {
            if (const int distance = nearest(outline, position); distance <= tolerance) {
                ++candidate.support;
                candidate.spread += distance;
            }
        }
        candidates.push_back(candidate);
    }
    std::ranges::sort(candidates, [](const Candidate& lhs, const Candidate& rhs) {
        return std::tuple{-lhs.support, lhs.spread, lhs.position} <
               std::tuple{-rhs.support, rhs.spread, rhs.position};
    });

    std::vector<int> kept;
    for (const Candidate& candidate : candidates) {
        if (candidate.support < std::max(needed, 1)) {
            break;  // the rest have less support still
        }
        const bool near_kept = std::ranges::any_of(
            kept, [&](int boundary) { return std::abs(boundary - candidate.position) <= tolerance; });
        if (!near_kept) {
            kept.push_back(candidate.position);
        }
    }
    std::ranges::sort(kept);
    return kept;
}

}  // namespace rr::reason

#include <rr/reason/boundaries.hpp>

#include <vector>

#include <gtest/gtest.h>

namespace rr::reason {
namespace {

using Outlines = std::vector<std::vector<int>>;

TEST(Boundaries, OutlinesThatAgreeWithinTheToleranceGiveOneBoundary) {
    const Outlines outlines{{10, 40}, {11, 40}, {12, 41}};
    EXPECT_EQ(boundary_consensus(outlines, 2, 0.5), (std::vector<int>{11, 40}))
        << "each at the position closest to all three";
}

TEST(Boundaries, ABoundaryTooFewOutlinesPlaceIsDropped) {
    EXPECT_EQ(boundary_consensus(Outlines{{10}, {10, 25}, {10}}, 2, 0.5), (std::vector<int>{10}));
    EXPECT_EQ(boundary_consensus(Outlines{{10, 30}, {10}, {10, 31}}, 2, 1.0), (std::vector<int>{10}))
        << "a quorum of 1 needs every outline";
    EXPECT_EQ(boundary_consensus(Outlines{{10}, {30}}, 2, 0.5), (std::vector<int>{10, 30}))
        << "half of two outlines is one";
}

TEST(Boundaries, TheToleranceIsInSentences) {
    EXPECT_EQ(boundary_consensus(Outlines{{10}, {11}, {10}}, 0, 0.5), (std::vector<int>{10})) << "exact only";
    EXPECT_EQ(boundary_consensus(Outlines{{10}, {13}, {16}}, 3, 0.5), (std::vector<int>{13}));
    EXPECT_TRUE(boundary_consensus(Outlines{{10}, {14}, {18}}, 3, 1.0).empty())
        << "too far apart for all three";
}

TEST(Boundaries, AnEvenSplitGoesToTheEarlierPosition) {
    EXPECT_EQ(boundary_consensus(Outlines{{10}, {12}}, 2, 1.0), (std::vector<int>{10}));
}

TEST(Boundaries, OutlinesMayListTheirBoundariesInAnyOrder) {
    EXPECT_EQ(boundary_consensus(Outlines{{40, 10, 10}, {10, 40}}, 1, 1.0), (std::vector<int>{10, 40}));
    EXPECT_TRUE(boundary_consensus(Outlines{}, 2, 0.5).empty());
    EXPECT_TRUE(boundary_consensus(Outlines{{}, {}}, 2, 0.5).empty());
}

TEST(Boundaries, AQuorumIsExactDespiteFloatingPoint) {
    // 0.56 × 25 is 14.000000000000002 in floating point; fourteen outlines are enough.
    Outlines outlines(25);
    for (std::size_t outline = 0; outline < 14; ++outline) {
        outlines[outline] = {10};
    }
    EXPECT_EQ(boundary_consensus(outlines, 0, 0.56), (std::vector<int>{10}));
}

}  // namespace
}  // namespace rr::reason

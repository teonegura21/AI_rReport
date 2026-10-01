#include <rr/reason/select.hpp>

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>

namespace rr::reason {
namespace {

using testkit::unwrap;
using Keys = std::vector<std::vector<double>>;

TEST(Select, AnEarlierKeyOutweighsEveryLaterOne) {
    // (ledger violations, judged issues, minus the flow score)
    const Keys drafts{{1, 0, -10}, {0, 5, -1}, {0, 5, -2}};
    EXPECT_EQ(unwrap(select_lexicographic(drafts)), 2U)
        << "the fewest violations first; then the fewest issues; then the best flow";
    EXPECT_EQ(unwrap(select_lexicographic(Keys{{0, 5}, {1, 0}})), 0U)
        << "a later key never buys back an earlier one";
}

TEST(Select, AFullTieGoesToTheFirst) {
    EXPECT_EQ(unwrap(select_lexicographic(Keys{{1, 2}, {1, 2}, {1, 3}})), 0U);
    EXPECT_EQ(unwrap(select_lexicographic(Keys{{7}})), 0U);
    EXPECT_EQ(unwrap(select_lexicographic(Keys{{}, {}})), 0U) << "no criterion: the first";
}

TEST(Select, NotANumberIsTheWorst) {
    const double nan = std::nan("");
    EXPECT_EQ(unwrap(select_lexicographic(Keys{{nan}, {5}})), 1U);
    EXPECT_EQ(unwrap(select_lexicographic(Keys{{5}, {nan}})), 0U);
    EXPECT_EQ(unwrap(select_lexicographic(Keys{{nan, 9}, {nan, 1}})), 1U)
        << "two NaNs tie; the next key decides";
}

TEST(Select, RefusesWhatItCannotCompare) {
    EXPECT_ERRC(select_lexicographic(Keys{}), Errc::invalid_argument);
    EXPECT_ERRC(select_lexicographic(Keys{{1, 2}, {1}}), Errc::invalid_argument);
}

}  // namespace
}  // namespace rr::reason

#include <rr/core/secret.hpp>

#include <format>
#include <sstream>

#include <gtest/gtest.h>

namespace rr {
namespace {

TEST(Secret, NeverPrintsItsValue) {
    const Secret secret{"sk-live-123"};
    EXPECT_EQ(std::format("token={}", secret), "token=***");
    std::ostringstream out;
    out << secret;
    EXPECT_EQ(out.str(), "***");
}

TEST(Secret, ExposeReturnsTheValue) {
    const Secret secret{"abc"};
    EXPECT_EQ(secret.expose(), "abc");
    EXPECT_FALSE(secret.empty());
    EXPECT_TRUE(Secret{}.empty());
}

TEST(Secret, MatchesComparesWholeValue) {
    const Secret secret{"token-123"};
    EXPECT_TRUE(secret.matches("token-123"));
    EXPECT_FALSE(secret.matches("token-124"));
    EXPECT_FALSE(secret.matches("token-12"));
    EXPECT_FALSE(secret.matches(""));
}

TEST(Secret, MoveLeavesTheSourceEmpty) {
    Secret source{"short"};  // short enough for the small-string optimization
    Secret target{std::move(source)};
    EXPECT_EQ(target.expose(), "short");
    // Deliberate use after move: verifying that the source was wiped.
    EXPECT_TRUE(source.empty());  // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)

    Secret assigned;
    assigned = std::move(target);
    EXPECT_EQ(assigned.expose(), "short");
    EXPECT_TRUE(target.empty());  // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
}

TEST(Secret, CopiesAreIndependent) {
    const Secret original{"value"};
    Secret copy{original};
    EXPECT_EQ(copy.expose(), "value");
    copy = Secret{"other"};
    EXPECT_EQ(original.expose(), "value");
}

}  // namespace
}  // namespace rr

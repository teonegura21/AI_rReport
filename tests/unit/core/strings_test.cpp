#include <rr/core/strings.hpp>

#include <array>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace rr {
namespace {

TEST(Trim, RemovesAsciiWhitespaceOnBothEnds) {
    EXPECT_EQ(trim("  a b \t\r\n"), "a b");
    EXPECT_EQ(trim(""), "");
    EXPECT_EQ(trim(" \t "), "");
    EXPECT_EQ(trim("x"), "x");
}

TEST(Split, KeepsEmptyFields) {
    EXPECT_EQ(split("a,,b", ','), (std::vector<std::string_view>{"a", "", "b"}));
    EXPECT_EQ(split("", ','), (std::vector<std::string_view>{""}));
    EXPECT_EQ(split("a,", ','), (std::vector<std::string_view>{"a", ""}));
}

TEST(Join, InsertsSeparatorBetweenParts) {
    const std::array<std::string, 3> parts{"a", "b", "c"};
    EXPECT_EQ(join(parts, ", "), "a, b, c");
    EXPECT_EQ(join(std::span<const std::string>{}, ","), "");
}

TEST(Case, AsciiOnlyConversions) {
    EXPECT_EQ(to_lower_ascii("Content-Type"), "content-type");
    EXPECT_EQ(to_upper_ascii("runtime_url"), "RUNTIME_URL");
    EXPECT_TRUE(iequals_ascii("Authorization", "authorization"));
    EXPECT_FALSE(iequals_ascii("Authorization", "authorisation"));
    EXPECT_FALSE(iequals_ascii("a", "ab"));
}

TEST(ReplaceAll, ReplacesNonOverlappingOccurrences) {
    EXPECT_EQ(replace_all("a__b__c", "__", "."), "a.b.c");
    EXPECT_EQ(replace_all("aaa", "aa", "b"), "ba");
    EXPECT_EQ(replace_all("none", "x", "y"), "none");
}

}  // namespace
}  // namespace rr

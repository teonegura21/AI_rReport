#include <rr/core/random.hpp>

#include <gtest/gtest.h>

namespace rr {
namespace {

TEST(Random, HexHasRequestedLengthAndAlphabet) {
    const std::string token = random_hex(32).value();
    EXPECT_EQ(token.size(), 64U);
    EXPECT_EQ(token.find_first_not_of("0123456789abcdef"), std::string::npos);
}

TEST(Random, SuccessiveTokensDiffer) {
    // 256 random bits: a collision here means the generator is broken.
    EXPECT_NE(random_hex().value(), random_hex().value());
}

TEST(Random, ZeroBytes_IsEmpty) {
    EXPECT_EQ(random_hex(0).value(), "");
}

}  // namespace
}  // namespace rr

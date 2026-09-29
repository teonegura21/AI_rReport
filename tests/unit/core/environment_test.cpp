#include <rr/core/environment.hpp>

#include <algorithm>
#include <cstdlib>

#include <gtest/gtest.h>

namespace rr {
namespace {

TEST(Environment, SnapshotAgreesWithGetenv) {
    const auto variables = environment_snapshot();
    ASSERT_FALSE(variables.empty());
    // PATH is set in every environment the tests run in.
    const auto path = std::ranges::find(variables, std::string{"PATH"}, &EnvironmentVariable::first);
    ASSERT_NE(path, variables.end());
    const char* expected = std::getenv("PATH");  // NOLINT(concurrency-mt-unsafe) — single-threaded test
    ASSERT_NE(expected, nullptr);
    EXPECT_EQ(path->second, expected);
}

TEST(Environment, LooksUpSingleVariables) {
    const auto path = environment_variable("PATH");
    ASSERT_TRUE(path.has_value());
    EXPECT_FALSE(path->empty());
    EXPECT_EQ(environment_variable("RR_SURELY_UNSET_VARIABLE_4F2A"), std::nullopt);
    EXPECT_EQ(environment_variable("PAT"), std::nullopt) << "a prefix of a name is not a match";
}

}  // namespace
}  // namespace rr

#include <rr/core/result.hpp>

#include <string>

#include <gtest/gtest.h>

namespace rr {
namespace {

Result<int> parse_positive(int value) {
    if (value <= 0) {
        return fail(Errc::invalid_argument, "not positive");
    }
    return value;
}

Result<void> check_even(int value) {
    if (value % 2 != 0) {
        return fail(Errc::invalid_argument, "odd");
    }
    return ok();
}

// Exercises both macros: declaration form, assignment form, and void results.
Result<int> double_if_valid(int value) {
    RR_TRY_ASSIGN(const int positive, parse_positive(value));
    RR_TRY(check_even(positive));
    int doubled = 0;
    RR_TRY_ASSIGN(doubled, parse_positive(positive * 2));
    return doubled;
}

TEST(ResultMacros, SuccessPath_ProducesValue) {
    const Result<int> result = double_if_valid(4);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 8);
}

TEST(ResultMacros, TryAssignFailure_PropagatesTheOriginalError) {
    const Result<int> result = double_if_valid(-3);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), Errc::invalid_argument);
    EXPECT_EQ(result.error().message(), "not positive");
}

TEST(ResultMacros, TryFailure_PropagatesVoidResultErrors) {
    const Result<int> result = double_if_valid(3);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().message(), "odd");
}

TEST(ResultMacros, SeveralTryAssignsOnOneLine_DoNotCollide) {
    auto twice = []() -> Result<int> {
        // __COUNTER__ keeps the hidden temporaries distinct even on one line.
        RR_TRY_ASSIGN(const int a, parse_positive(1));
        RR_TRY_ASSIGN(const int b, parse_positive(2));
        return a + b;
    };
    EXPECT_EQ(twice().value(), 3);
}

TEST(WithContext, AddsContextOnlyOnFailure) {
    const Result<int> good = with_context(parse_positive(5), "while parsing");
    EXPECT_EQ(good.value(), 5);

    const Result<int> bad = with_context(parse_positive(0), "while parsing");
    ASSERT_FALSE(bad.has_value());
    ASSERT_EQ(bad.error().context().size(), 1U);
    EXPECT_EQ(bad.error().context()[0], "while parsing");
}

}  // namespace
}  // namespace rr

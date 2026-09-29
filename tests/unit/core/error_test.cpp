#include <rr/core/error.hpp>

#include <gtest/gtest.h>

#include <rr/core/result.hpp>

namespace rr {
namespace {

TEST(Errc, EveryClass_RoundTripsThroughItsName) {
    for (int value = 0; value <= static_cast<int>(Errc::internal); ++value) {
        const auto code = static_cast<Errc>(value);
        const std::string_view name = to_string(code);
        EXPECT_NE(name, "unknown");
        ASSERT_TRUE(errc_from_string(name).has_value()) << name;
        EXPECT_EQ(*errc_from_string(name), code);
    }
    EXPECT_FALSE(errc_from_string("no_such_class").has_value());
}

TEST(Errc, TransientClasses_AreExactlyTheRetryableOnes) {
    EXPECT_TRUE(is_transient(Errc::network));
    EXPECT_TRUE(is_transient(Errc::timeout));
    EXPECT_TRUE(is_transient(Errc::unavailable));
    EXPECT_TRUE(is_transient(Errc::rate_limited));
    EXPECT_FALSE(is_transient(Errc::invalid_argument));
    EXPECT_FALSE(is_transient(Errc::model_output_invalid));
    EXPECT_FALSE(is_transient(Errc::cancelled));
    EXPECT_FALSE(is_transient(Errc::internal));
}

TEST(Error, WithContext_AppendsLayersInOrder) {
    const Error error = Error{Errc::timeout, "no response"}
                            .with_context("while calling /health")
                            .with_context("while starting pod");
    ASSERT_EQ(error.context().size(), 2U);
    EXPECT_EQ(error.context()[0], "while calling /health");
    EXPECT_EQ(error.context()[1], "while starting pod");
}

TEST(Error, Describe_IncludesClassMessageContextAndLocation) {
    const Error error = Error{Errc::rate_limited, "slow down"}
                            .with_context("while fetching")
                            .with_retry_after(std::chrono::milliseconds{1500});
    const std::string text = error.describe();
    EXPECT_NE(text.find("rate_limited: slow down"), std::string::npos) << text;
    EXPECT_NE(text.find("(while fetching)"), std::string::npos) << text;
    EXPECT_NE(text.find("[retry after 1500 ms]"), std::string::npos) << text;
    EXPECT_NE(text.find("error_test.cpp:"), std::string::npos) << text;
    ASSERT_TRUE(error.retry_after().has_value());
    EXPECT_EQ(error.retry_after()->count(), 1500);
}

TEST(Error, Fail_RecordsTheCallersLocation) {
    const int line = __LINE__ + 1;
    const std::unexpected<Error> failure = fail(Errc::not_found, "missing");
    EXPECT_EQ(failure.error().where().line(), static_cast<std::uint_least32_t>(line));
    EXPECT_TRUE(failure.error().where().file_name() != nullptr);
}

}  // namespace
}  // namespace rr

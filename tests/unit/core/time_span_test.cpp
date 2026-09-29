#include <rr/core/time_span.hpp>

#include <gtest/gtest.h>

namespace rr {
namespace {

using std::chrono::milliseconds;

TimeSpan span(long long start, long long end) {
    return TimeSpan::make(milliseconds{start}, milliseconds{end}).value();
}

TEST(TimeSpan, Make_RejectsNegativeStartAndInvertedBounds) {
    EXPECT_EQ(TimeSpan::make(milliseconds{-1}, milliseconds{5}).error().code(), Errc::invalid_argument);
    EXPECT_EQ(TimeSpan::make(milliseconds{10}, milliseconds{5}).error().code(), Errc::invalid_argument);
    EXPECT_TRUE(TimeSpan::make(milliseconds{5}, milliseconds{5}).value().empty());
}

TEST(TimeSpan, Contains_IsHalfOpen) {
    const TimeSpan s = span(1000, 2000);
    EXPECT_TRUE(s.contains(milliseconds{1000}));
    EXPECT_TRUE(s.contains(milliseconds{1999}));
    EXPECT_FALSE(s.contains(milliseconds{2000}));
    EXPECT_FALSE(s.contains(milliseconds{999}));
}

TEST(TimeSpan, OverlapAndMerge) {
    const TimeSpan a = span(0, 1000);
    const TimeSpan b = span(500, 1500);
    const TimeSpan c = span(1000, 2000);  // touches a at 1000 but shares no instant
    EXPECT_TRUE(a.overlaps(b));
    EXPECT_FALSE(a.overlaps(c));
    EXPECT_EQ(a.overlap(b), milliseconds{500});
    EXPECT_EQ(a.overlap(c), milliseconds{0});
    EXPECT_EQ(a.merged(c), span(0, 2000));
    EXPECT_EQ(b.duration(), milliseconds{1000});
}

TEST(Timestamps, FormatLikeAVideoPlayer) {
    EXPECT_EQ(format_timestamp(milliseconds{0}), "0:00");
    EXPECT_EQ(format_timestamp(milliseconds{245'000}), "4:05");
    EXPECT_EQ(format_timestamp(milliseconds{3'723'999}), "1:02:03");
    EXPECT_EQ(format_timestamp(milliseconds{-5}), "0:00");
}

TEST(Timestamps, ParseAcceptsPlayerFormats) {
    EXPECT_EQ(parse_timestamp("42").value(), milliseconds{42'000});
    EXPECT_EQ(parse_timestamp("4:05").value(), milliseconds{245'000});
    EXPECT_EQ(parse_timestamp("1:02:03").value(), milliseconds{3'723'000});
    EXPECT_EQ(parse_timestamp("1:02:03.5").value(), milliseconds{3'723'500});
    EXPECT_EQ(parse_timestamp("0:07.025").value(), milliseconds{7'025});
}

TEST(Timestamps, ParseRejectsMalformedText) {
    for (const char* bad : {"", ":", "1:60", "a:10", "1:2:3:4", "1.", "1.2345", "-3", "1:-2"}) {
        EXPECT_FALSE(parse_timestamp(bad).has_value()) << bad;
    }
}

TEST(Timestamps, FormatThenParse_RoundTripsWholeSeconds) {
    for (const long long ms : {0LL, 59'000LL, 60'000LL, 3'599'000LL, 3'600'000LL, 36'000'000LL}) {
        EXPECT_EQ(parse_timestamp(format_timestamp(milliseconds{ms})).value(), milliseconds{ms});
    }
}

}  // namespace
}  // namespace rr

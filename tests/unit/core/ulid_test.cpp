#include <rr/core/ulid.hpp>

#include <set>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <rr/core/clock.hpp>

namespace rr {
namespace {

bool is_crockford(std::string_view text) {
    return text.find_first_not_of("0123456789ABCDEFGHJKMNPQRSTVWXYZ") == std::string_view::npos;
}

TEST(Ulid, HasCanonicalShape) {
    const FakeClock clock;
    UlidGenerator generator{clock};
    const std::string id = generator.next();
    EXPECT_EQ(id.size(), 26U);
    EXPECT_TRUE(is_crockford(id)) << id;
    // 128 bits in 130 bits of base32: the first character carries 3 bits.
    EXPECT_LE(id.front(), '7') << id;
}

TEST(Ulid, IdsWithinOneMillisecond_AreStrictlyIncreasing) {
    const FakeClock clock;  // never advanced: every ID shares one timestamp
    UlidGenerator generator{clock};
    std::string previous = generator.next();
    for (int index = 0; index < 1000; ++index) {
        std::string current = generator.next();
        EXPECT_LT(previous, current);
        previous = std::move(current);
    }
}

TEST(Ulid, LaterMilliseconds_SortLater) {
    FakeClock clock;
    UlidGenerator generator{clock};
    const std::string first = generator.next();
    clock.advance(std::chrono::milliseconds{1});
    const std::string second = generator.next();
    EXPECT_LT(first, second);
    // The first 10 characters encode the timestamp.
    EXPECT_NE(first.substr(0, 10), second.substr(0, 10));
}

TEST(Ulid, KnownTimestamp_EncodesToExpectedPrefix) {
    // FakeClock starts at 2026-01-01T00:00:00Z = 1767225600000 ms since epoch.
    // 1767225600000 in Crockford base32, 10 characters wide: 01KDVDNA00
    // (computed independently with Python when this test was written).
    const FakeClock clock;
    UlidGenerator generator{clock};
    EXPECT_EQ(generator.next().substr(0, 10), "01KDVDNA00");
}

TEST(Ulid, GlobalGenerator_ProducesUniqueIds) {
    std::set<std::string> ids;
    for (int index = 0; index < 500; ++index) {
        ids.insert(new_ulid());
    }
    EXPECT_EQ(ids.size(), 500U);
}

}  // namespace
}  // namespace rr

#include <rr/supervisor/watchdog.hpp>

#include <gtest/gtest.h>

namespace rr::supervisor {
namespace {

using namespace std::chrono_literals;

TEST(Watchdog, StopsAfterTheIdlePeriod) {
    FakeClock clock;
    const Watchdog watchdog{clock, 20min};
    EXPECT_EQ(watchdog.stops_in(), 20min);
    clock.advance(19min);
    EXPECT_FALSE(watchdog.should_stop());
    EXPECT_EQ(watchdog.stops_in(), 1min);
    clock.advance(1min);
    EXPECT_TRUE(watchdog.should_stop());
    EXPECT_EQ(watchdog.idle_for(), 20min);
}

TEST(Watchdog, ActivityAndHeartbeatsResetTheTimer) {
    FakeClock clock;
    Watchdog watchdog{clock, 10min};
    clock.advance(9min);
    watchdog.activity();  // e.g. the hub's heartbeat
    clock.advance(9min);
    EXPECT_FALSE(watchdog.should_stop());
    EXPECT_EQ(watchdog.idle_for(), 9min);
}

TEST(Watchdog, ARunningRequestIsNeverIdle) {
    FakeClock clock;
    Watchdog watchdog{clock, 10min};
    {
        const Watchdog::InFlight generation = watchdog.begin_request();
        clock.advance(2h);  // a very long generation
        EXPECT_FALSE(watchdog.should_stop());
        EXPECT_EQ(watchdog.stops_in(), std::nullopt);
        EXPECT_EQ(watchdog.in_flight(), 1);
    }
    EXPECT_EQ(watchdog.in_flight(), 0);
    EXPECT_EQ(watchdog.stops_in(), 10min) << "idleness counts from the end of the request";
}

TEST(Watchdog, CanBeDisabled) {
    FakeClock clock;
    const Watchdog watchdog{clock, std::nullopt};
    clock.advance(24h);
    EXPECT_FALSE(watchdog.should_stop());
    EXPECT_EQ(watchdog.stops_in(), std::nullopt);
    EXPECT_EQ(watchdog.idle_for(), 24h);
}

}  // namespace
}  // namespace rr::supervisor

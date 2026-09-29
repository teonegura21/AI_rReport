#include <rr/net/retry.hpp>

#include <chrono>
#include <stop_token>
#include <thread>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>

namespace rr::net {
namespace {

using namespace std::chrono_literals;
using ::testing::HasSubstr;

const Error kTransient{Errc::unavailable, "pod restarting"};

TEST(Backoff, GrowsExponentiallyUpToTheCap) {
    const RetryPolicy policy{.max_attempts = 10, .initial_delay = 500ms, .max_delay = 4s, .multiplier = 2.0};
    constexpr double kAlmostOne = 0.999999;
    EXPECT_EQ(backoff_delay(policy, 1, kTransient, 0.0), 0ms) << "full jitter starts at zero";
    // Ceilings 500, 1000, 2000, 4000 ms, then capped; × 0.999999, truncated.
    EXPECT_EQ(backoff_delay(policy, 1, kTransient, kAlmostOne), 499ms);
    EXPECT_EQ(backoff_delay(policy, 2, kTransient, kAlmostOne), 999ms);
    EXPECT_EQ(backoff_delay(policy, 3, kTransient, kAlmostOne), 1999ms);
    EXPECT_EQ(backoff_delay(policy, 4, kTransient, kAlmostOne), 3999ms);
    EXPECT_EQ(backoff_delay(policy, 5, kTransient, kAlmostOne), 3999ms) << "capped";
    EXPECT_EQ(backoff_delay(policy, 1000, kTransient, kAlmostOne), 3999ms) << "no overflow";
    EXPECT_EQ(backoff_delay(policy, 3, kTransient, 0.5), 1000ms);
}

TEST(Backoff, AServerHintWinsButIsCapped) {
    const RetryPolicy policy{.max_retry_after = 60s};
    EXPECT_EQ(backoff_delay(policy, 1, Error{kTransient}.with_retry_after(7s), 0.0), 7s);
    EXPECT_EQ(backoff_delay(policy, 1, Error{kTransient}.with_retry_after(1h), 0.9), 60s);
}

TEST(RandomUnit, StaysInItsRange) {
    for (int sample = 0; sample < 1000; ++sample) {
        const double unit = random_unit();
        EXPECT_GE(unit, 0.0);
        EXPECT_LT(unit, 1.0);
    }
}

// Records pauses instead of sleeping.
struct RecordingPause {
    std::vector<std::chrono::milliseconds> delays;
    Pause pause() {
        return [this](std::chrono::milliseconds delay, const std::stop_token&) {
            delays.push_back(delay);
            return true;
        };
    }
};

TEST(WithRetries, ReturnsAtOnceOnSuccess) {
    RecordingPause pause;
    int calls = 0;
    const Result<int> result =
        with_retries(RetryPolicy{}, [&](int) -> Result<int> { return ++calls; }, {}, pause.pause());
    EXPECT_EQ(result.value(), 1);
    EXPECT_TRUE(pause.delays.empty());
}

TEST(WithRetries, RetriesTransientFailuresWithGrowingPauses) {
    RecordingPause pause;
    std::vector<int> attempts;
    const RetryPolicy policy{.max_attempts = 4, .initial_delay = 100ms, .max_delay = 10s};
    const Result<std::string> result = with_retries(
        policy,
        [&](int attempt) -> Result<std::string> {
            attempts.push_back(attempt);
            if (attempt < 3) {
                return fail(Errc::network, "connection reset");
            }
            return std::string{"done"};
        },
        {}, pause.pause());
    EXPECT_EQ(result.value(), "done");
    EXPECT_EQ(attempts, (std::vector<int>{1, 2, 3}));
    ASSERT_EQ(pause.delays.size(), 2U);
    EXPECT_LE(pause.delays[0], 100ms);
    EXPECT_LE(pause.delays[1], 200ms);
}

TEST(WithRetries, NeverRetriesPermanentFailures) {
    RecordingPause pause;
    int calls = 0;
    const auto result = with_retries(
        RetryPolicy{},
        [&](int) -> Result<void> {
            ++calls;
            return fail(Errc::invalid_argument, "bad request");
        },
        {}, pause.pause());
    EXPECT_ERRC(result, Errc::invalid_argument);
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(pause.delays.empty());
}

TEST(WithRetries, GivesUpAfterTheLastAttemptWithTheLastError) {
    RecordingPause pause;
    int calls = 0;
    const auto result = with_retries(
        RetryPolicy{.max_attempts = 3},
        [&](int attempt) -> Result<void> {
            ++calls;
            return fail(Errc::timeout, std::format("attempt {} timed out", attempt));
        },
        {}, pause.pause());
    EXPECT_ERRC(result, Errc::timeout);
    EXPECT_EQ(calls, 3);
    EXPECT_EQ(result.error().message(), "attempt 3 timed out");
    EXPECT_THAT(result.error().describe(), HasSubstr("gave up after 3 attempts"));
    EXPECT_ERRC(with_retries(
                    RetryPolicy::none(), [](int) -> Result<void> { return fail(Errc::network, "down"); }, {},
                    pause.pause()),
                Errc::network);
}

TEST(WithRetries, HonoursAServerRetryAfter) {
    RecordingPause pause;
    const auto result = with_retries(
        RetryPolicy{.max_attempts = 2},
        [](int attempt) -> Result<void> {
            if (attempt == 1) {
                return fail(Error{Errc::rate_limited, "429"}.with_retry_after(1500ms));
            }
            return ok();
        },
        {}, pause.pause());
    EXPECT_OK(result);
    EXPECT_EQ(pause.delays, (std::vector<std::chrono::milliseconds>{1500ms}));
}

TEST(WithRetries, AStopDuringThePauseCancels) {
    // Non-const: request_stop() is a non-const member in the C++ standard.
    std::stop_source stop;  // NOLINT(misc-const-correctness)
    stop.request_stop();
    int calls = 0;
    const auto started = std::chrono::steady_clock::now();
    const auto result = with_retries(
        RetryPolicy{.initial_delay = 10s, .max_delay = 10s},
        [&](int) -> Result<void> {
            ++calls;
            return fail(Errc::unavailable, "busy");
        },
        stop.get_token());  // the real, interruptible pause
    EXPECT_ERRC(result, Errc::cancelled);
    EXPECT_EQ(calls, 1);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
}

TEST(PauseFor, SleepsOrReturnsEarlyOnStop) {
    EXPECT_TRUE(pause_for(10ms, {}));
    std::stop_source stop;
    const std::jthread stopper{[&stop] {
        std::this_thread::sleep_for(50ms);
        stop.request_stop();
    }};
    const auto started = std::chrono::steady_clock::now();
    EXPECT_FALSE(pause_for(10s, stop.get_token()));
    EXPECT_LT(std::chrono::steady_clock::now() - started, 5s);
}

}  // namespace
}  // namespace rr::net

#include <rr/core/limiter.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace rr {
namespace {

using namespace std::chrono_literals;

// Polls a condition for up to two seconds; tests use it instead of fixed
// sleeps so they are fast on a quiet machine and still robust on a busy one.
template <class Predicate>
bool eventually(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

TEST(Limiter, NeverExceedsCapacity) {
    Limiter limiter{3};
    std::atomic<int> current{0};
    std::atomic<int> peak{0};
    std::vector<std::jthread> threads;
    threads.reserve(12);
    for (int index = 0; index < 12; ++index) {
        threads.emplace_back([&] {
            auto permit = limiter.acquire({});
            ASSERT_TRUE(permit.has_value());
            const int now = current.fetch_add(1) + 1;
            int expected = peak.load();
            while (now > expected && !peak.compare_exchange_weak(expected, now)) {
            }
            std::this_thread::sleep_for(2ms);
            current.fetch_sub(1);
        });
    }
    threads.clear();  // join
    EXPECT_LE(peak.load(), 3);
    EXPECT_EQ(limiter.in_use(), 0U);
}

TEST(Limiter, HigherPriorityIsServedFirst_ThenFifo) {
    Limiter limiter{1};
    auto blocker = limiter.acquire({}).value();

    std::mutex order_mutex;
    std::vector<int> order;
    auto waiter = [&](int label, int priority) {
        return std::jthread([&, label, priority] {
            auto permit = limiter.acquire({}, Priority{priority});
            ASSERT_TRUE(permit.has_value());
            const std::lock_guard lock(order_mutex);
            order.push_back(label);
        });
    };

    // Queue: low(1), low(2), high(3). Wait until each is enqueued so the
    // sequence numbers are deterministic.
    auto t1 = waiter(1, 0);
    ASSERT_TRUE(eventually([&] { return limiter.waiting() == 1; }));
    auto t2 = waiter(2, 0);
    ASSERT_TRUE(eventually([&] { return limiter.waiting() == 2; }));
    auto t3 = waiter(3, 10);
    ASSERT_TRUE(eventually([&] { return limiter.waiting() == 3; }));

    blocker.release();
    t1.join();
    t2.join();
    t3.join();
    EXPECT_EQ(order, (std::vector<int>{3, 1, 2}));
}

TEST(Limiter, StopRequestCancelsAWaiter) {
    Limiter limiter{1};
    auto held = limiter.acquire({}).value();
    std::stop_source source;
    auto result = std::async(std::launch::async, [&] { return limiter.acquire(source.get_token()); });
    ASSERT_TRUE(eventually([&] { return limiter.waiting() == 1; }));
    source.request_stop();
    const auto outcome = result.get();
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code(), Errc::cancelled);
    EXPECT_EQ(limiter.waiting(), 0U);
}

TEST(Limiter, AcquireForTimesOut) {
    Limiter limiter{1};
    auto held = limiter.acquire({}).value();
    const auto outcome = limiter.acquire_for(20ms, {});
    ASSERT_FALSE(outcome.has_value());
    EXPECT_EQ(outcome.error().code(), Errc::timeout);
}

TEST(Limiter, TryAcquireDoesNotJumpTheQueue) {
    Limiter limiter{1};
    auto held = limiter.acquire({}).value();
    EXPECT_FALSE(limiter.try_acquire().has_value());  // full
    const std::jthread waiter([&] { auto permit = limiter.acquire({}); });
    ASSERT_TRUE(eventually([&] { return limiter.waiting() == 1; }));
    held.release();
    // A slot is (briefly) free, but the queued waiter has precedence.
    ASSERT_TRUE(eventually([&] { return limiter.waiting() == 0; }));
}

TEST(Limiter, RaisingCapacityWakesWaiters) {
    Limiter limiter{0};
    auto result = std::async(std::launch::async, [&] { return limiter.acquire({}); });
    ASSERT_TRUE(eventually([&] { return limiter.waiting() == 1; }));
    limiter.set_capacity(1);
    EXPECT_TRUE(result.get().has_value());
}

TEST(Limiter, PermitsMoveAndReleaseOnce) {
    Limiter limiter{1};
    Limiter::Permit outer;
    {
        auto permit = limiter.acquire({}).value();
        outer = std::move(permit);
        // Deliberate use after move: verifying the moved-from state.
        EXPECT_FALSE(permit.valid());  // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    }
    EXPECT_EQ(limiter.in_use(), 1U);  // moved-from permit released nothing
    outer.release();
    outer.release();  // idempotent
    EXPECT_EQ(limiter.in_use(), 0U);
}

}  // namespace
}  // namespace rr

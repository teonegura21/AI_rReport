#include <rr/core/thread_pool.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <latch>
#include <stdexcept>

#include <gtest/gtest.h>

namespace rr {
namespace {

using namespace std::chrono_literals;

TEST(ThreadPool, SubmitReturnsTheTaskResult) {
    ThreadPool pool{2};
    auto answer = pool.submit([] { return 42; });
    auto text = pool.submit([](const std::stop_token&) { return std::string{"with token"}; });
    EXPECT_EQ(answer.get(), 42);
    EXPECT_EQ(text.get(), "with token");
}

TEST(ThreadPool, ExceptionsTravelThroughTheFuture) {
    ThreadPool pool{1};
    auto failing = pool.submit([]() -> int { throw std::runtime_error{"boom"}; });
    EXPECT_THROW(failing.get(), std::runtime_error);
    // The worker survived and keeps serving.
    EXPECT_EQ(pool.submit([] { return 1; }).get(), 1);
}

TEST(ThreadPool, PostedExceptionsGoToTheErrorHandler) {
    std::promise<std::string> reported;
    ThreadPool pool{1, "test", [&](std::exception_ptr error) {
                        try {
                            std::rethrow_exception(std::move(error));
                        } catch (const std::exception& e) {
                            reported.set_value(e.what());
                        }
                    }};
    pool.post([] { throw std::logic_error{"posted failure"}; });
    auto future = reported.get_future();
    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    EXPECT_EQ(future.get(), "posted failure");
}

TEST(ThreadPool, TasksRunConcurrently) {
    constexpr int kTasks = 4;
    ThreadPool pool{kTasks};
    // Every task waits for all others to start: this only finishes if all
    // four run at the same time.
    std::latch all_started{kTasks};
    std::vector<std::future<void>> futures;
    futures.reserve(kTasks);
    for (int index = 0; index < kTasks; ++index) {
        futures.push_back(pool.submit([&] {
            all_started.count_down();
            all_started.wait();
        }));
    }
    for (auto& future : futures) {
        ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    }
}

TEST(ThreadPool, WaitIdleWaitsForQueuedAndRunningWork) {
    ThreadPool pool{2};
    std::atomic<int> done{0};
    for (int index = 0; index < 50; ++index) {
        pool.post([&] { done.fetch_add(1); });
    }
    pool.wait_idle();
    EXPECT_EQ(done.load(), 50);
}

TEST(ThreadPool, DestructorSignalsStopAndDropsQueuedTasks) {
    std::future<void> running;
    std::future<int> queued;
    std::promise<void> started;
    {
        ThreadPool pool{1};
        running = pool.submit([&](const std::stop_token& stop) {
            started.set_value();
            while (!stop.stop_requested()) {
                std::this_thread::sleep_for(1ms);
            }
        });
        queued = pool.submit([] { return 7; });  // cannot start: the only worker is busy
        started.get_future().wait();
    }
    EXPECT_NO_THROW(running.get());  // observed the stop request and returned
    try {
        (void)queued.get();
        FAIL() << "a dropped task must not produce a value";
    } catch (const std::future_error& error) {
        EXPECT_EQ(error.code(), std::future_errc::broken_promise);
    }
}

}  // namespace
}  // namespace rr

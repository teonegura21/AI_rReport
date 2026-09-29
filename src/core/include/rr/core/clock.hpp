// rr/core/clock.hpp — injectable clocks, so time-dependent logic is testable.
//
// Why this exists
//   Idle shutdown, retries, heartbeats, ULIDs and timeouts all depend on time.
//   Code that calls std::chrono::*::now() directly can only be tested by
//   sleeping, which makes tests slow and flaky. Components take a `Clock&`
//   instead; production passes SystemClock, tests pass FakeClock and advance
//   it explicitly.
//
// Thread-safety: SystemClock is stateless. FakeClock is thread-safe (atomic
//   state), so a test thread can advance it while code under test reads it.
#pragma once

#include <atomic>
#include <chrono>

namespace rr {

class Clock {
public:
    Clock() = default;
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;
    Clock(Clock&&) = delete;
    Clock& operator=(Clock&&) = delete;
    virtual ~Clock() = default;

    // Wall-clock time: for timestamps stored in records and for ULIDs.
    [[nodiscard]] virtual std::chrono::system_clock::time_point now() const = 0;

    // Monotonic time: for measuring intervals, deadlines and idle periods.
    [[nodiscard]] virtual std::chrono::steady_clock::time_point steady_now() const = 0;
};

class SystemClock final : public Clock {
public:
    [[nodiscard]] std::chrono::system_clock::time_point now() const override {
        return std::chrono::system_clock::now();
    }

    [[nodiscard]] std::chrono::steady_clock::time_point steady_now() const override {
        return std::chrono::steady_clock::now();
    }

    // A process-wide instance for production wiring (stateless, so sharing is safe).
    [[nodiscard]] static SystemClock& instance() noexcept {
        static SystemClock clock;
        return clock;
    }
};

// A manually driven clock for tests. Starts at a fixed, recognizable instant
// (2026-01-01T00:00:00Z) so failures print readable timestamps.
class FakeClock final : public Clock {
public:
    FakeClock() = default;

    [[nodiscard]] std::chrono::system_clock::time_point now() const override {
        return std::chrono::system_clock::time_point{std::chrono::nanoseconds{wall_ns_.load()}};
    }

    [[nodiscard]] std::chrono::steady_clock::time_point steady_now() const override {
        return std::chrono::steady_clock::time_point{std::chrono::nanoseconds{steady_ns_.load()}};
    }

    // Moves both clocks forward by `delta` (which must be non-negative).
    void advance(std::chrono::nanoseconds delta) noexcept {
        wall_ns_.fetch_add(delta.count());
        steady_ns_.fetch_add(delta.count());
    }

private:
    static constexpr long long kStartWallNs = 1'767'225'600LL * 1'000'000'000LL;  // 2026-01-01T00:00:00Z
    std::atomic<long long> wall_ns_{kStartWallNs};
    std::atomic<long long> steady_ns_{0};
};

}  // namespace rr

// rr/supervisor/watchdog.hpp — when should this pod stop itself?
//
// Why this exists
//   A GPU pod costs money every minute it runs (plan §4.3). It must stop on
//   its own when nobody needs it, including when the hub that wanted it is
//   gone (the laptop went to sleep, the hub crashed): a dead-man switch.
//
//   One rule covers both cases. Activity is any authenticated request, and
//   the hub's periodic heartbeat while it wants the pod. When there has been
//   no activity for `idle_stop`, and no request is in flight (a long
//   generation is activity even when nothing new arrives), the pod stops.
//   A hub that stops heartbeating, whether on purpose or by dying, lets the
//   pod stop.
//
// Thread-safety: all members are thread-safe.
#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>

#include <rr/core/clock.hpp>

namespace rr::supervisor {

class Watchdog {
public:
    // Keeps a request "in flight" (never idle) for the guard's lifetime.
    class InFlight {
    public:
        explicit InFlight(Watchdog& watchdog) noexcept;
        InFlight(InFlight&& other) noexcept;
        InFlight& operator=(InFlight&&) = delete;
        InFlight(const InFlight&) = delete;
        InFlight& operator=(const InFlight&) = delete;
        ~InFlight();

    private:
        Watchdog* watchdog_;
    };

    // `idle_stop`: std::nullopt disables stopping (development, tests).
    Watchdog(const Clock& clock, std::optional<std::chrono::milliseconds> idle_stop) noexcept;

    // Something happened: a request arrived or the hub sent a heartbeat.
    void activity() noexcept;
    // Marks a request as running until the returned guard is destroyed.
    [[nodiscard]] InFlight begin_request() noexcept;

    [[nodiscard]] std::chrono::milliseconds idle_for() const noexcept;
    [[nodiscard]] int in_flight() const noexcept { return in_flight_.load(); }
    // Time left before stopping; std::nullopt when disabled or busy.
    [[nodiscard]] std::optional<std::chrono::milliseconds> stops_in() const noexcept;
    [[nodiscard]] bool should_stop() const noexcept;

private:
    [[nodiscard]] std::chrono::steady_clock::time_point last_activity() const noexcept;

    const Clock& clock_;
    std::optional<std::chrono::milliseconds> idle_stop_;
    std::atomic<std::chrono::steady_clock::rep> last_activity_ns_;
    std::atomic<int> in_flight_{0};
};

}  // namespace rr::supervisor

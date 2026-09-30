#include <rr/supervisor/watchdog.hpp>

#include <algorithm>
#include <utility>

namespace rr::supervisor {

Watchdog::Watchdog(const Clock& clock, std::optional<std::chrono::milliseconds> idle_stop) noexcept
    : clock_(clock), idle_stop_(idle_stop), last_activity_ns_(clock.steady_now().time_since_epoch().count()) {}

void Watchdog::activity() noexcept {
    last_activity_ns_.store(clock_.steady_now().time_since_epoch().count());
}

Watchdog::InFlight Watchdog::begin_request() noexcept {
    return InFlight{*this};
}

Watchdog::InFlight::InFlight(Watchdog& watchdog) noexcept : watchdog_(&watchdog) {
    watchdog_->in_flight_.fetch_add(1);
    watchdog_->activity();
}

Watchdog::InFlight::InFlight(InFlight&& other) noexcept : watchdog_(std::exchange(other.watchdog_, nullptr)) {}

Watchdog::InFlight::~InFlight() {
    if (watchdog_ != nullptr) {
        watchdog_->activity();  // the idle period starts when the request ends, not when it began
        watchdog_->in_flight_.fetch_sub(1);
    }
}

std::chrono::steady_clock::time_point Watchdog::last_activity() const noexcept {
    return std::chrono::steady_clock::time_point{std::chrono::steady_clock::duration{last_activity_ns_.load()}};
}

std::chrono::milliseconds Watchdog::idle_for() const noexcept {
    if (in_flight_.load() > 0) {
        return std::chrono::milliseconds{0};
    }
    const auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(clock_.steady_now() - last_activity());
    return std::max(idle, std::chrono::milliseconds{0});
}

std::optional<std::chrono::milliseconds> Watchdog::stops_in() const noexcept {
    if (!idle_stop_.has_value() || in_flight_.load() > 0) {
        return std::nullopt;
    }
    return std::max(*idle_stop_ - idle_for(), std::chrono::milliseconds{0});
}

bool Watchdog::should_stop() const noexcept {
    const auto remaining = stops_in();
    return remaining.has_value() && remaining->count() == 0;
}

}  // namespace rr::supervisor

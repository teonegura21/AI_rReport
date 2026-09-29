#include <rr/net/retry.hpp>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <random>

namespace rr::net {

std::chrono::milliseconds backoff_delay(const RetryPolicy& policy, int attempt, const Error& error,
                                        double unit) noexcept {
    if (error.retry_after().has_value()) {
        return std::clamp(*error.retry_after(), std::chrono::milliseconds{0}, policy.max_retry_after);
    }
    // initial · multiplier^(attempt-1), computed in double and capped before
    // converting back, so large attempt numbers cannot overflow.
    const double ceiling_ms =
        std::min(static_cast<double>(policy.max_delay.count()),
                 static_cast<double>(policy.initial_delay.count()) *
                     std::pow(policy.multiplier, static_cast<double>(std::max(attempt, 1) - 1)));
    const double clamped_unit = std::clamp(unit, 0.0, 1.0);
    return std::chrono::milliseconds{static_cast<std::int64_t>(ceiling_ms * clamped_unit)};
}

double random_unit() noexcept {
    static thread_local std::mt19937_64 engine{std::random_device{}()};
    return std::uniform_real_distribution<double>{0.0, 1.0}(engine);
}

bool pause_for(std::chrono::milliseconds delay, const std::stop_token& stop) {
    // condition_variable_any can wait on a stop_token: a stop request wakes
    // the wait immediately instead of after the full delay.
    std::mutex mutex;
    std::condition_variable_any wake;
    std::unique_lock lock(mutex);
    // Nothing else ever notifies: the wait ends on timeout or on a stop request.
    (void)wake.wait_for(lock, stop, delay, [] { return false; });
    return !stop.stop_requested();
}

}  // namespace rr::net

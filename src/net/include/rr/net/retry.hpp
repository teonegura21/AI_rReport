// rr/net/retry.hpp — retrying transient failures with backoff.
//
// Why this exists
//   Networks drop connections, pods restart, model APIs answer 429 and 529.
//   The right response to a *transient* error is to try again later; the
//   wrong ones are giving up at once, or hammering the peer. The rules
//   (ADR-0007):
//     * Only transient errors are retried (Error::transient(): network,
//       timeout, unavailable, rate_limited). A bad request stays bad.
//     * Exponential backoff with "full jitter": the pause before retry n is
//       uniformly random in [0, min(max_delay, initial · multiplierⁿ⁻¹)].
//       Randomness spreads out many clients that failed together, so a
//       recovering server is not hit by synchronized waves.
//     * A server's Retry-After hint wins over the computed pause (capped at
//       max_retry_after): the server knows when it will be ready.
//     * Waiting is interruptible: a stop request ends the pause at once.
//
// Thread-safety: stateless apart from a thread-local random engine.
#pragma once

#include <chrono>
#include <format>
#include <functional>
#include <stop_token>
#include <type_traits>
#include <utility>

#include <rr/core/result.hpp>

namespace rr::net {

struct RetryPolicy {
    int max_attempts = 4;  // including the first
    std::chrono::milliseconds initial_delay{500};
    std::chrono::milliseconds max_delay{30'000};
    double multiplier = 2.0;
    std::chrono::milliseconds max_retry_after{300'000};  // cap for server Retry-After hints

    // A policy that never retries.
    [[nodiscard]] static RetryPolicy none() noexcept { return RetryPolicy{.max_attempts = 1}; }
};

// The pause before the retry that follows failed attempt `attempt` (1-based).
// `unit` is uniformly random in [0, 1). Pure, so tests can pin it down.
[[nodiscard]] std::chrono::milliseconds backoff_delay(const RetryPolicy& policy, int attempt,
                                                      const Error& error, double unit) noexcept;

// A uniformly random number in [0, 1) from a thread-local engine.
[[nodiscard]] double random_unit() noexcept;

// Sleeps for `delay`; returns false as soon as `stop` is requested.
using Pause = std::function<bool(std::chrono::milliseconds delay, const std::stop_token& stop)>;
[[nodiscard]] bool pause_for(std::chrono::milliseconds delay, const std::stop_token& stop);

// Calls `attempt(n)` (n = 1, 2, …) until it succeeds, fails with an error
// that is not transient, or `policy.max_attempts` is used up; returns the
// last result. A stop request during a pause returns Errc::cancelled.
// `attempt` is taken by value: it is called repeatedly, never consumed.
template <class F>
auto with_retries(const RetryPolicy& policy, F attempt, const std::stop_token& stop = {},
                  const Pause& pause = pause_for) -> std::invoke_result_t<F&, int> {
    using R = std::remove_cvref_t<std::invoke_result_t<F&, int>>;
    static_assert(is_result_v<R>, "with_retries(): the attempt must return rr::Result<T>");
    for (int number = 1;; ++number) {
        R result = std::invoke(attempt, number);
        if (result.has_value() || !result.error().transient()) {
            return result;
        }
        if (number >= policy.max_attempts) {
            if (number == 1) {
                return result;
            }
            return fail(
                std::move(result).error().with_context(std::format("gave up after {} attempts", number)));
        }
        const auto delay = backoff_delay(policy, number, result.error(), random_unit());
        if (!pause(delay, stop)) {
            return fail(Errc::cancelled,
                        std::format("cancelled while waiting to retry: {}", result.error().describe()));
        }
    }
}

}  // namespace rr::net

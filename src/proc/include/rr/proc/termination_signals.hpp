// rr/proc/termination_signals.hpp — SIGINT, SIGTERM and SIGHUP as a stop request.
//
// Why this exists
//   A server must stop cleanly when asked: SIGTERM from `docker stop` or
//   RunPod stopping the pod, SIGINT from Ctrl-C. Handling signals with a
//   handler function is a minefield: almost nothing is async-signal-safe
//   (no locks, no allocation, no logging). Instead, the signals are blocked
//   in every thread and taken synchronously by one thread with
//   sigtimedwait(), which turns them into an ordinary std::stop_source
//   request that the rest of the program handles like any cancellation.
//
//   * Construct it first in main(), before any other thread exists: threads
//     inherit the signal mask, so no other thread ever receives the signals.
//     Child processes do not inherit it: rr_proc resets the mask of every
//     process it spawns (ADR-0013).
//   * It also works for PID 1 in a container, where a signal without a
//     handler would be ignored: the kernel never ignores a blocked signal
//     (it may be waited for).
//   * The waiting thread uses short sigtimedwait() slices, so it notices its
//     own stop request and can be joined; plain sigwait() cannot be woken.
//   * Only the first signal counts. Once it has arrived, the signals stay
//     blocked: a second Ctrl-C during a graceful shutdown does not kill the
//     process halfway through it.
//
// Thread-safety: one instance per process; its members are thread-safe.
#pragma once

#include <atomic>
#include <csignal>
#include <stop_token>
#include <thread>

namespace rr::proc {

class TerminationSignals {
public:
    // Blocks SIGINT, SIGTERM and SIGHUP in the calling thread (so in every
    // thread created after it) and starts waiting for them.
    TerminationSignals();
    TerminationSignals(const TerminationSignals&) = delete;
    TerminationSignals& operator=(const TerminationSignals&) = delete;
    TerminationSignals(TerminationSignals&&) = delete;
    TerminationSignals& operator=(TerminationSignals&&) = delete;
    ~TerminationSignals();  // stops waiting; the signals stay blocked

    // Requested when a signal arrives.
    [[nodiscard]] std::stop_token stop_token() const noexcept { return source_.get_token(); }
    // The signal that arrived, 0 while none has.
    [[nodiscard]] int received() const noexcept { return received_.load(); }
    // Blocks until a signal arrives.
    void wait() const;

private:
    sigset_t signals_{};
    std::stop_source source_;
    std::atomic<int> received_{0};
    std::jthread waiter_;  // declared last: joined before the members above go away
};

}  // namespace rr::proc

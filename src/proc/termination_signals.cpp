#include <rr/proc/termination_signals.hpp>

#include <condition_variable>
#include <mutex>

#include <pthread.h>

namespace rr::proc {

namespace {

// How long one sigtimedwait() slice lasts: the waiter's reaction time to its
// own stop request (a signal itself is taken at once).
constexpr long kSliceNanoseconds = 200'000'000;

}  // namespace

TerminationSignals::TerminationSignals() {
    sigemptyset(&signals_);
    sigaddset(&signals_, SIGINT);
    sigaddset(&signals_, SIGTERM);
    sigaddset(&signals_, SIGHUP);
    (void)pthread_sigmask(SIG_BLOCK, &signals_, nullptr);  // cannot fail with a valid set
    waiter_ = std::jthread{[this](const std::stop_token& stop) {
        const timespec slice{.tv_sec = 0, .tv_nsec = kSliceNanoseconds};
        while (!stop.stop_requested()) {
            const int signal_number = sigtimedwait(&signals_, nullptr, &slice);
            if (signal_number > 0) {
                received_.store(signal_number);
                source_.request_stop();
                return;
            }
            // -1: the slice ended (EAGAIN) or a stray signal interrupted it (EINTR).
        }
    }};
}

TerminationSignals::~TerminationSignals() = default;  // the jthread requests stop and joins

void TerminationSignals::wait() const {
    std::mutex mutex;
    std::condition_variable_any woken;
    std::unique_lock lock(mutex);
    (void)woken.wait(lock, source_.get_token(), [] { return false; });  // returns once stop is requested
}

}  // namespace rr::proc

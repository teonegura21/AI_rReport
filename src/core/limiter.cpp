#include <rr/core/limiter.hpp>

#include <format>

#include <rr/core/assert.hpp>

namespace rr {

// ---------------------------------------------------------------------------
// Permit
// ---------------------------------------------------------------------------
Limiter::Permit::Permit(Permit&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}

Limiter::Permit& Limiter::Permit::operator=(Permit&& other) noexcept {
    if (this != &other) {
        release();
        owner_ = std::exchange(other.owner_, nullptr);
    }
    return *this;
}

Limiter::Permit::~Permit() {
    release();
}

void Limiter::Permit::release() noexcept {
    if (owner_ != nullptr) {
        std::exchange(owner_, nullptr)->release_one();
    }
}

// ---------------------------------------------------------------------------
// Limiter
// ---------------------------------------------------------------------------
Limiter::Limiter(std::size_t capacity) : capacity_(capacity) {}

Limiter::~Limiter() {
    const std::lock_guard lock(mutex_);
    RR_ASSERT(in_use_ == 0, "Limiter destroyed while permits are still held");
    RR_ASSERT(waiters_.empty(), "Limiter destroyed while threads are waiting");
}

template <class WaitFn>
Result<Limiter::Permit> Limiter::acquire_impl(Priority priority, const WaitFn& wait) {
    std::unique_lock lock(mutex_);
    const Ticket ticket{.priority = priority.value, .sequence = next_sequence_++};
    waiters_.insert(ticket);

    // A waiter may proceed only when it heads the queue and a slot is free.
    // Checking the head (not just capacity) is what makes priorities and FIFO
    // order hold even when several slots free up at once.
    auto my_turn = [&] { return *waiters_.begin() == ticket && in_use_ < capacity_; };
    const bool granted = wait(lock, my_turn);

    waiters_.erase(ticket);
    if (!granted) {
        // Our departure may have promoted a new head that can proceed now.
        lock.unlock();
        cv_.notify_all();
        return std::unexpected(Error{Errc::cancelled, "wait for a limiter slot was cancelled"});
    }
    ++in_use_;
    lock.unlock();
    // Another slot may still be free for the new head of the queue.
    cv_.notify_all();
    return Permit{this};
}

Result<Limiter::Permit> Limiter::acquire(std::stop_token stop, Priority priority) {
    return acquire_impl(priority, [&](std::unique_lock<std::mutex>& lock, auto& predicate) {
        return cv_.wait(lock, stop, predicate);
    });
}

Result<Limiter::Permit> Limiter::acquire_for(std::chrono::milliseconds timeout, std::stop_token stop,
                                             Priority priority) {
    bool timed_out = false;
    auto result = acquire_impl(priority, [&](std::unique_lock<std::mutex>& lock, auto& predicate) {
        const bool granted = cv_.wait_for(lock, stop, timeout, predicate);
        timed_out = !granted && !stop.stop_requested();
        return granted;
    });
    if (!result && timed_out) {
        return fail(Errc::timeout, std::format("no limiter slot became free within {} ms", timeout.count()));
    }
    return result;
}

std::optional<Limiter::Permit> Limiter::try_acquire() {
    const std::lock_guard lock(mutex_);
    if (!waiters_.empty() || in_use_ >= capacity_) {
        return std::nullopt;
    }
    ++in_use_;
    return Permit{this};
}

void Limiter::set_capacity(std::size_t capacity) {
    {
        const std::lock_guard lock(mutex_);
        capacity_ = capacity;
    }
    cv_.notify_all();
}

std::size_t Limiter::capacity() const {
    const std::lock_guard lock(mutex_);
    return capacity_;
}

std::size_t Limiter::in_use() const {
    const std::lock_guard lock(mutex_);
    return in_use_;
}

std::size_t Limiter::waiting() const {
    const std::lock_guard lock(mutex_);
    return waiters_.size();
}

void Limiter::release_one() noexcept {
    {
        const std::lock_guard lock(mutex_);
        RR_ASSERT(in_use_ > 0, "Limiter permit released twice");
        --in_use_;
    }
    cv_.notify_all();
}

}  // namespace rr

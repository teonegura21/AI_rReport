#include <rr/core/limiter.hpp>

#include <format>
#include <string>
#include <utility>

#include <rr/core/assert.hpp>

namespace rr {

// ---------------------------------------------------------------------------
// Permit
// ---------------------------------------------------------------------------
Limiter::Permit::Permit(Permit&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr))
    , slots_(std::exchange(other.slots_, 0)) {}

Limiter::Permit& Limiter::Permit::operator=(Permit&& other) noexcept {
    if (this != &other) {
        release();
        owner_ = std::exchange(other.owner_, nullptr);
        slots_ = std::exchange(other.slots_, 0);
    }
    return *this;
}

Limiter::Permit::~Permit() {
    release();
}

void Limiter::Permit::release() noexcept {
    if (owner_ != nullptr) {
        std::exchange(owner_, nullptr)->release_slots(std::exchange(slots_, 0));
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

namespace {

std::unexpected<Error> too_wide(std::size_t slots, std::size_t capacity) {
    return std::unexpected(Error{
        Errc::invalid_argument,
        slots == 0 ? std::string{"a permit holds at least one slot"}
                   : std::format("{} slots at once were asked of a limiter of {}: it would wait forever",
                                 slots, capacity)});
}

// Whether `slots` can never be granted at `capacity`. A capacity of 0 is a
// closed limiter, which may open: there, every request waits.
bool never_fits(std::size_t slots, std::size_t capacity) {
    return capacity > 0 && slots > capacity;
}

}  // namespace

template <class WaitFn>
Result<Limiter::Permit> Limiter::acquire_impl(Priority priority, std::size_t slots, const WaitFn& wait) {
    std::unique_lock lock(mutex_);
    if (slots == 0) {  // the wait below would grant it at once
        return too_wide(slots, capacity_);
    }
    const Ticket ticket{.priority = priority.value, .sequence = next_sequence_++};
    waiters_.insert(ticket);

    // A waiter may proceed only when it heads the queue and its slots are all
    // free. Checking the head (not just the free slots) is what makes
    // priorities and FIFO order hold even when several slots free up at once,
    // and what keeps narrow requests from overtaking a wide one. It also
    // stops waiting when its slots can never fit: at once (the predicate is
    // checked before the first wait), or when the capacity shrinks below them.
    auto ready = [&] {
        return never_fits(slots, capacity_) || (*waiters_.begin() == ticket && in_use_ + slots <= capacity_);
    };
    const bool woken = wait(lock, ready);

    waiters_.erase(ticket);
    if (!woken || never_fits(slots, capacity_)) {
        // Our departure may have promoted a new head that can proceed now.
        const std::size_t capacity = capacity_;
        lock.unlock();
        cv_.notify_all();
        if (woken) {
            return too_wide(slots, capacity);
        }
        return std::unexpected(Error{Errc::cancelled, "wait for a limiter slot was cancelled"});
    }
    in_use_ += slots;
    lock.unlock();
    // Other slots may still be free for the new head of the queue.
    cv_.notify_all();
    return Permit{this, slots};
}

Result<Limiter::Permit> Limiter::acquire(std::stop_token stop, Priority priority, Slots slots) {
    return acquire_impl(priority, slots.count, [&](std::unique_lock<std::mutex>& lock, auto& predicate) {
        return cv_.wait(lock, stop, predicate);
    });
}

Result<Limiter::Permit> Limiter::acquire_for(std::chrono::milliseconds timeout, std::stop_token stop,
                                             Priority priority, Slots slots) {
    bool timed_out = false;
    auto result =
        acquire_impl(priority, slots.count, [&](std::unique_lock<std::mutex>& lock, auto& predicate) {
            const bool granted = cv_.wait_for(lock, stop, timeout, predicate);
            timed_out = !granted && !stop.stop_requested();
            return granted;
        });
    if (!result && timed_out) {
        return fail(Errc::timeout, std::format("no limiter slot became free within {} ms", timeout.count()));
    }
    return result;
}

std::optional<Limiter::Permit> Limiter::try_acquire(Slots slots) {
    const std::lock_guard lock(mutex_);
    if (slots.count == 0 || !waiters_.empty() || in_use_ + slots.count > capacity_) {
        return std::nullopt;
    }
    in_use_ += slots.count;
    return Permit{this, slots.count};
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

void Limiter::release_slots(std::size_t slots) noexcept {
    {
        const std::lock_guard lock(mutex_);
        RR_ASSERT(in_use_ >= slots, "Limiter permit released twice");
        in_use_ -= slots;
    }
    cv_.notify_all();
}

}  // namespace rr

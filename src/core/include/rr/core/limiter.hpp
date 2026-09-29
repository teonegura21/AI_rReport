// rr/core/limiter.hpp — bounded concurrency with priorities and cancellation.
//
// Why this exists
//   Each model endpoint on the runtime has a fixed number of parallel slots
//   (llama-server's -np). The hub must never send more concurrent requests than
//   an endpoint can serve: excess requests would queue inside llama-server, out
//   of our control, where cancellation and prioritization are impossible.
//   Limiter is the hub-side gate (ADR-0010, ADR-0011):
//
//     auto permit = limiter.acquire(stop, Priority{10});   // blocks until a slot is free
//     if (!permit) return fail(std::move(permit).error());  // cancelled or timed out
//     call_model(...);                                      // permit released at scope exit
//
// Scheduling
//   Waiters are served strictly by priority (higher first); equal priorities
//   are first-come, first-served. This lets the commander's interactive chat
//   overtake a batch of background verification missions.
//
// Thread-safety: all members are thread-safe. The Limiter must outlive every
//   Permit it issued (asserted in the destructor).
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>

#include <rr/core/result.hpp>

namespace rr {

struct Priority {
    int value = 0;
};

class Limiter {
public:
    // RAII handle for one acquired slot; move-only.
    class Permit {
    public:
        Permit() = default;
        Permit(const Permit&) = delete;
        Permit& operator=(const Permit&) = delete;
        Permit(Permit&& other) noexcept;
        Permit& operator=(Permit&& other) noexcept;
        ~Permit();

        // Releases the slot early (idempotent).
        void release() noexcept;

        [[nodiscard]] bool valid() const noexcept { return owner_ != nullptr; }

    private:
        friend class Limiter;

        explicit Permit(Limiter* owner) noexcept : owner_(owner) {}

        Limiter* owner_ = nullptr;
    };

    explicit Limiter(std::size_t capacity);
    Limiter(const Limiter&) = delete;
    Limiter& operator=(const Limiter&) = delete;
    Limiter(Limiter&&) = delete;
    Limiter& operator=(Limiter&&) = delete;
    ~Limiter();

    // Waits for a slot. Fails with Errc::cancelled if `stop` is requested first.
    [[nodiscard]] Result<Permit> acquire(std::stop_token stop, Priority priority = {});

    // As acquire(), but fails with Errc::timeout after `timeout`.
    [[nodiscard]] Result<Permit> acquire_for(std::chrono::milliseconds timeout, std::stop_token stop,
                                             Priority priority = {});

    // A slot if one is free *and* nobody is waiting (never jumps the queue).
    [[nodiscard]] std::optional<Permit> try_acquire();

    // Changes the capacity (e.g. when a GPU profile changes slot counts).
    // Shrinking never revokes held permits; it only delays new ones.
    void set_capacity(std::size_t capacity);

    [[nodiscard]] std::size_t capacity() const;
    [[nodiscard]] std::size_t in_use() const;
    [[nodiscard]] std::size_t waiting() const;

private:
    struct Ticket {
        int priority;
        std::uint64_t sequence;
        friend bool operator==(const Ticket&, const Ticket&) = default;

        // Highest priority first; FIFO within a priority.
        friend bool operator<(const Ticket& lhs, const Ticket& rhs) noexcept {
            if (lhs.priority != rhs.priority) {
                return lhs.priority > rhs.priority;
            }
            return lhs.sequence < rhs.sequence;
        }
    };

    template <class WaitFn>
    Result<Permit> acquire_impl(Priority priority, const WaitFn& wait);

    void release_one() noexcept;

    mutable std::mutex mutex_;        // guards everything below
    std::condition_variable_any cv_;  // signalled whenever a slot frees or the queue head changes
    std::size_t capacity_;
    std::size_t in_use_ = 0;
    std::uint64_t next_sequence_ = 0;
    std::set<Ticket> waiters_;
};

}  // namespace rr

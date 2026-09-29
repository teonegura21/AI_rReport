// rr/core/ulid.hpp — time-sortable unique identifiers (ULID).
//
// Why this exists
//   Jobs, analyses, missions and research steps need IDs that are unique
//   without coordination *and* sort by creation time. That second property
//   makes "latest first" queries cheap and log lines easy to follow.
//   A ULID is 48 bits of milliseconds since the Unix epoch plus 80 random
//   bits, encoded as 26 characters of Crockford base32 (no I, L, O, U):
//
//     01J9ZQ3M7X8K4T2VYB6NRCQW5D
//     └─ time ─┘└──── random ────┘
//
//   Within the same millisecond, this generator increments the random part
//   instead of drawing a new one ("monotonic ULIDs"), so IDs created in a
//   burst still sort in creation order.
//
// Thread-safety: UlidGenerator is thread-safe (internal mutex).
#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>

#include <rr/core/clock.hpp>

namespace rr {

class UlidGenerator {
public:
    explicit UlidGenerator(const Clock& clock);

    // A new ULID, strictly greater than every ULID previously returned by this
    // generator (as long as the clock does not jump backwards by more than the
    // last issued timestamp).
    [[nodiscard]] std::string next();

private:
    const Clock& clock_;
    std::mutex mutex_;        // guards everything below
    std::mt19937_64 random_;  // IDs are not secrets; see rr::random_bytes for those
    std::uint64_t last_ms_ = 0;
    std::array<std::uint8_t, 10> last_random_{};  // 80 bits, big-endian
};

// Convenience: a ULID from a process-wide generator on the system clock.
[[nodiscard]] std::string new_ulid();

}  // namespace rr

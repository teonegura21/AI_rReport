// rr/core/time_span.hpp — positions and intervals on a video's timeline.
//
// Why this exists
//   Everything in rReport points back into a video: sentences, atoms, frames,
//   anchors (ADR-0009). A TimeSpan is a half-open interval [start, end) in
//   milliseconds with its invariants enforced at construction, so no code ever
//   handles a span that ends before it starts.
//
// Thread-safety: a regular value type.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include <rr/core/result.hpp>

namespace rr {

using Millis = std::chrono::milliseconds;

class TimeSpan {
public:
    // Validates 0 <= start <= end.
    [[nodiscard]] static Result<TimeSpan> make(Millis start, Millis end);

    [[nodiscard]] constexpr Millis start() const noexcept { return start_; }
    [[nodiscard]] constexpr Millis end() const noexcept { return end_; }
    [[nodiscard]] constexpr Millis duration() const noexcept { return end_ - start_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return start_ == end_; }

    // Half-open: `end` itself is not contained.
    [[nodiscard]] constexpr bool contains(Millis point) const noexcept {
        return point >= start_ && point < end_;
    }

    [[nodiscard]] constexpr bool overlaps(const TimeSpan& other) const noexcept {
        return start_ < other.end_ && other.start_ < end_;
    }

    // Length of the intersection; zero when the spans do not overlap.
    [[nodiscard]] constexpr Millis overlap(const TimeSpan& other) const noexcept {
        const Millis lo = start_ > other.start_ ? start_ : other.start_;
        const Millis hi = end_ < other.end_ ? end_ : other.end_;
        return hi > lo ? hi - lo : Millis{0};
    }

    // The smallest span containing both.
    [[nodiscard]] constexpr TimeSpan merged(const TimeSpan& other) const noexcept {
        return TimeSpan{start_ < other.start_ ? start_ : other.start_, end_ > other.end_ ? end_ : other.end_};
    }

    friend constexpr bool operator==(const TimeSpan&, const TimeSpan&) = default;

private:
    constexpr TimeSpan(Millis start, Millis end) noexcept : start_(start), end_(end) {}

    Millis start_;
    Millis end_;
};

// Formats a timeline position the way video players show it:
//   "4:05" below one hour, "1:02:03" from one hour on. Negative values clamp to 0.
[[nodiscard]] std::string format_timestamp(Millis position);

// Parses "SS", "M:SS", "H:MM:SS" (optionally with ".mmm") into milliseconds.
[[nodiscard]] Result<Millis> parse_timestamp(std::string_view text);

}  // namespace rr

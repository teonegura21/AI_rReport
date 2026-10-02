// shown.hpp — numbers as the eval's reports show them (private to rr_eval).
//
// Shares are shown to a tenth of a point (0.962); counts of hundreds or
// more, tokens, calls or minutes, whole (123456): three decimals there would
// be noise, and none on a share would hide every difference worth seeing.
#pragma once

#include <cmath>
#include <format>
#include <string>

namespace rr::eval::detail {

inline constexpr double kWholeFrom = 100;

[[nodiscard]] inline std::string shown(double value) {
    return std::abs(value) >= kWholeFrom ? std::format("{:.0f}", value) : std::format("{:.3f}", value);
}

// The same, always signed: "+0.012", "-0.004".
[[nodiscard]] inline std::string signed_shown(double value) {
    return std::abs(value) >= kWholeFrom ? std::format("{:+.0f}", value) : std::format("{:+.3f}", value);
}

}  // namespace rr::eval::detail

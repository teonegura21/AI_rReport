#include <rr/core/time_span.hpp>

#include <charconv>
#include <format>
#include <limits>
#include <string_view>
#include <vector>

namespace rr {

Result<TimeSpan> TimeSpan::make(Millis start, Millis end) {
    if (start < Millis{0}) {
        return fail(Errc::invalid_argument, std::format("time span starts before 0 ({} ms)", start.count()));
    }
    if (end < start) {
        return fail(Errc::invalid_argument, std::format("time span ends before it starts ({} ms < {} ms)",
                                                        end.count(), start.count()));
    }
    return TimeSpan{start, end};
}

std::string format_timestamp(Millis position) {
    const long long total_seconds = position.count() > 0 ? position.count() / 1000 : 0;
    const long long hours = total_seconds / 3600;
    const long long minutes = (total_seconds % 3600) / 60;
    const long long seconds = total_seconds % 60;
    if (hours > 0) {
        return std::format("{}:{:02}:{:02}", hours, minutes, seconds);
    }
    return std::format("{}:{:02}", minutes, seconds);
}

namespace {

// Parses one numeric field of a timestamp; `original` is the full input,
// quoted in the error so the user sees what they typed.
Result<long long> parse_component(std::string_view component, std::string_view original) {
    long long value = 0;
    const char* first = component.data();
    const char* last = component.data() + component.size();
    const auto [end, error] = std::from_chars(first, last, value);
    if (component.empty() || error != std::errc{} || end != last || value < 0) {
        return fail(Errc::invalid_argument, std::format("invalid timestamp '{}'", original));
    }
    return value;
}

}  // namespace

Result<Millis> parse_timestamp(std::string_view text) {
    // Split off an optional fractional part: "1:02:03.250" → "1:02:03" + "250".
    std::string_view clock_part = text;
    long long millis = 0;
    if (const auto dot = text.find('.'); dot != std::string_view::npos) {
        const std::string_view fraction = text.substr(dot + 1);
        if (fraction.empty() || fraction.size() > 3) {
            return fail(Errc::invalid_argument, std::format("invalid timestamp '{}'", text));
        }
        RR_TRY_ASSIGN(millis, parse_component(fraction, text));
        for (std::size_t digits = fraction.size(); digits < 3; ++digits) {
            millis *= 10;  // ".5" means 500 ms
        }
        clock_part = text.substr(0, dot);
    }

    std::vector<std::string_view> parts;
    std::size_t begin = 0;
    while (true) {
        const std::size_t colon = clock_part.find(':', begin);
        parts.push_back(clock_part.substr(begin, colon - begin));
        if (colon == std::string_view::npos) {
            break;
        }
        begin = colon + 1;
    }
    if (parts.size() > 3) {
        return fail(Errc::invalid_argument, std::format("invalid timestamp '{}'", text));
    }

    // The most seconds whose milliseconds a Millis can count: beyond, the
    // arithmetic below would overflow ("99999999999999999:00:00").
    constexpr long long kMostSeconds = (std::numeric_limits<long long>::max() / 1000) - 1;
    long long seconds = 0;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        RR_TRY_ASSIGN(const long long value, parse_component(parts[index], text));
        // Every component after the first is bounded by 60 (minutes, seconds).
        const bool past_sixty = index > 0 && value >= 60;
        if (past_sixty || seconds > (kMostSeconds - value) / 60) {
            return fail(Errc::invalid_argument, std::format("invalid timestamp '{}'", text));
        }
        seconds = (seconds * 60) + value;
    }
    return Millis{(seconds * 1000) + millis};
}

}  // namespace rr

// fields.hpp — reading an artifact's fields, once its schema has passed
// (private to rr_knowledge).
//
// A schema checks types, enumerations and required fields. What it leaves to
// code: integers that fit their C++ type, and the values of an enumeration
// mapped to their C++ enum. Both are here, so every reader states them once.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include <rr/core/result.hpp>
#include <rr/json/json.hpp>

namespace rr::knowledge::fields {

// An enumeration's values, written as its JSON names, in the enum's order.
template <typename Enum, std::size_t N>
[[nodiscard]] constexpr std::string_view name_of(Enum value,
                                                 const std::array<std::string_view, N>& names) noexcept {
    const auto index = static_cast<std::size_t>(value);
    return index < N ? names[index] : std::string_view{"?"};
}

template <typename Enum, std::size_t N>
[[nodiscard]] Result<Enum> named(std::string_view name, const std::array<std::string_view, N>& names) {
    for (std::size_t index = 0; index < N; ++index) {
        if (names[index] == name) {
            return static_cast<Enum>(index);
        }
    }
    return fail(Errc::invalid_argument, std::format("'{}' is no value of this field", name));
}

// A non-negative integer that fits an int64.
[[nodiscard]] inline Result<std::int64_t> count(const json::Json& value, std::string_view field) {
    if (value.is_number_unsigned() &&
        value.get<std::uint64_t>() <= std::numeric_limits<std::int64_t>::max()) {
        return static_cast<std::int64_t>(value.get<std::uint64_t>());
    }
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0) {
        return value.get<std::int64_t>();
    }
    return fail(Errc::invalid_argument, std::format("{} is not a count", field));
}

// A string or null.
[[nodiscard]] inline std::optional<std::string> optional_string(const json::Json& value) {
    return value.is_null() ? std::nullopt : std::optional<std::string>{value.get<std::string>()};
}

[[nodiscard]] inline json::Json json_or_null(const std::optional<std::string>& value) {
    return value.has_value() ? json::Json(*value) : json::Json(nullptr);
}

}  // namespace rr::knowledge::fields

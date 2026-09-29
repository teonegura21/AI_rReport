// rr/db/value.hpp — a value bound to an SQL statement parameter.
//
// Why this exists
//   SQLite stores five kinds of values: NULL, INTEGER (signed 64-bit), REAL,
//   TEXT and BLOB. `Value` holds exactly one of them, and its implicit
//   converting constructors decide, once and for the whole code base, how C++
//   types map onto them:
//
//     C++ type                                  SQLite
//     nullptr, empty std::optional              NULL
//     bool                                      INTEGER 1 / 0
//     integers (not char types)                 INTEGER; an unsigned 64-bit
//                                               value above INT64_MAX aborts
//     float, double                             REAL
//     std::string, std::string_view, char*      TEXT (copied)
//     Blob, std::span<const std::byte>          BLOB (copied)
//     StrongId<Tag, Rep>                        whatever Rep maps to
//
//   Deliberately *not* convertible: characters ('x' is not the number 120),
//   pointers (they would silently become booleans) and durations (a number
//   without its unit is a bug waiting to happen).
//
// Thread-safety: a plain value type.
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <rr/core/assert.hpp>
#include <rr/core/strong_id.hpp>

namespace rr::db {

using Blob = std::vector<std::byte>;

namespace detail {
// Character types are integral in C++, but binding 'x' as the integer 120 is
// never what a caller meant; they must spell out a string instead.
template <class T>
concept character = std::same_as<T, char> || std::same_as<T, wchar_t> || std::same_as<T, char8_t> ||
                    std::same_as<T, char16_t> || std::same_as<T, char32_t>;
}  // namespace detail

// A statement parameter. The converting constructors accept the C++ types
// that map onto SQLite's storage classes (NULL, INTEGER, REAL, TEXT, BLOB), so
// parameter lists read naturally:
//
//   tx.execute("INSERT INTO t VALUES (?1, ?2, ?3, ?4)",
//              {video_id, std::string_view{title}, duration_ms, std::optional<std::string>{}});
//
// Constructors are implicit on purpose: they are what makes `{a, b, c}` work.
class Value {
public:
    using Storage = std::variant<std::nullptr_t, std::int64_t, double, std::string, Blob>;

    Value() noexcept = default;  // NULL
    Value(std::nullptr_t) noexcept {}

    // SQLite has no boolean type; 1 and 0 are the convention. A template so
    // that only a real bool matches: a plain Value(bool) would also accept
    // any pointer, through the implicit pointer-to-bool conversion.
    template <std::same_as<bool> T>
    Value(T flag) noexcept : storage_(std::int64_t{flag ? 1 : 0}) {}

    template <std::integral T>
        requires(!std::same_as<T, bool> && !detail::character<T>)
    Value(T number) noexcept : storage_(to_int64(number)) {}

    template <std::floating_point T>
    Value(T number) noexcept : storage_(static_cast<double>(number)) {}

    Value(std::string text) noexcept : storage_(std::move(text)) {}
    Value(std::string_view text) : storage_(std::string{text}) {}
    Value(const char* text) : storage_(std::string{text}) {}
    Value(Blob bytes) noexcept : storage_(std::move(bytes)) {}
    Value(std::span<const std::byte> bytes) : storage_(Blob(bytes.begin(), bytes.end())) {}

    template <class Tag, class Rep>
    Value(const StrongId<Tag, Rep>& id) : Value(id.value()) {}

    // An empty optional binds NULL — the natural mapping for nullable columns.
    template <class T>
    Value(const std::optional<T>& value) : Value(value.has_value() ? Value{*value} : Value{}) {}

    [[nodiscard]] const Storage& storage() const noexcept { return storage_; }
    [[nodiscard]] bool is_null() const noexcept { return std::holds_alternative<std::nullptr_t>(storage_); }

private:
    template <std::integral T>
    static std::int64_t to_int64(T number) noexcept {
        if constexpr (std::is_unsigned_v<T> && sizeof(T) >= sizeof(std::int64_t)) {
            // SQLite integers are signed 64-bit; wrapping would silently store a
            // different number. Values this large belong in TEXT or BLOB.
            RR_ASSERT(std::in_range<std::int64_t>(number),
                      "unsigned value does not fit SQLite's signed 64-bit INTEGER");
        }
        return static_cast<std::int64_t>(number);
    }

    Storage storage_;
};

}  // namespace rr::db

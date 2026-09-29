// rr/testkit/result.hpp — GoogleTest helpers for Result<T>.
//
// Why this exists
//   `EXPECT_TRUE(result.has_value())` tells you *that* something failed, and
//   `result.value()` on an error throws std::bad_expected_access, which tells
//   you nothing. These helpers print the error's full description (code,
//   message, context chain, source location), so a failing test explains
//   itself:
//
//     ASSERT_OK(db.migrate(kMigrations));
//     EXPECT_ERRC(db.migrate(newer), Errc::conflict) << "downgrades are refused";
//     Database db = unwrap(Database::open(options));
//
//   The macros are built on GoogleTest's predicate-formatter assertions, so
//   they behave like native ones: `<<` appends a message, each argument is
//   evaluated exactly once, and the failure shows the original expression.
//
// Thread-safety: stateless.
#pragma once

#include <ostream>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

#include <rr/core/result.hpp>

namespace rr {

// Lets GoogleTest print error codes by name ("conflict") instead of a number.
// Found by argument-dependent lookup, so it must live in Errc's namespace.
inline void PrintTo(Errc code, std::ostream* out) {
    *out << to_string(code);
}

}  // namespace rr

namespace rr::testkit {

// The value of a successful Result; otherwise throws with the error's
// description, which GoogleTest reports as the reason the test failed.
template <class T>
T unwrap(Result<T> result) {
    if (!result.has_value()) {
        throw std::runtime_error("unexpected error: " + result.error().describe());
    }
    if constexpr (!std::is_void_v<T>) {
        return std::move(result).value();
    }
}

namespace detail {

template <class T>
::testing::AssertionResult format_ok(const char* expression, const Result<T>& result) {
    if (result.has_value()) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << expression << "\n  failed: " << result.error().describe();
}

template <class T>
::testing::AssertionResult format_errc(const char* expression, const char* /*errc_expression*/,
                                       const Result<T>& result, Errc expected) {
    if (result.has_value()) {
        return ::testing::AssertionFailure()
               << expression << "\n  succeeded; expected error: " << to_string(expected);
    }
    if (result.error().code() != expected) {
        return ::testing::AssertionFailure() << expression << "\n  expected error: " << to_string(expected)
                                             << "\n  actual error:   " << result.error().describe();
    }
    return ::testing::AssertionSuccess();
}

}  // namespace detail
}  // namespace rr::testkit

// NOLINTBEGIN(cppcoreguidelines-macro-usage) — assertions must be macros to report the caller's line.

// EXPECT_OK(r) / ASSERT_OK(r): `r` (any Result) succeeded; otherwise print its error.
#define EXPECT_OK(expr) EXPECT_PRED_FORMAT1(::rr::testkit::detail::format_ok, expr)
#define ASSERT_OK(expr) ASSERT_PRED_FORMAT1(::rr::testkit::detail::format_ok, expr)

// EXPECT_ERRC(r, code): `r` failed with the given error code.
#define EXPECT_ERRC(expr, errc) EXPECT_PRED_FORMAT2(::rr::testkit::detail::format_errc, expr, errc)
#define ASSERT_ERRC(expr, errc) ASSERT_PRED_FORMAT2(::rr::testkit::detail::format_errc, expr, errc)

// NOLINTEND(cppcoreguidelines-macro-usage)

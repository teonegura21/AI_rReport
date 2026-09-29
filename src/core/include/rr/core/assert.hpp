// rr/core/assert.hpp — RR_ASSERT: always-on invariant checks.
//
// Why this exists
//   Errors that callers can handle travel as Result values (ADR-0007). A
//   *broken invariant* is different: it means the program itself is wrong, and
//   continuing would corrupt state (for example, writing an artifact whose
//   cache key does not match its content). RR_ASSERT stops the process
//   immediately with the expression, a message and the source location.
//
//   Unlike <cassert>, it stays enabled in release builds: an invariant that is
//   worth writing down is worth checking in production.
//
// Use it for programmer errors only — never to validate input.
#pragma once

#include <source_location>
#include <string_view>

namespace rr::detail {

// Prints the failure to stderr and aborts. Never returns.
[[noreturn]] void assertion_failed(const char* expression, std::string_view message,
                                   std::source_location where) noexcept;

}  // namespace rr::detail

// NOLINTBEGIN(cppcoreguidelines-macro-usage) — must capture the expression text.
#define RR_ASSERT(condition, message)                                                                 \
    do {                                                                                              \
        if (!(condition)) [[unlikely]] {                                                              \
            ::rr::detail::assertion_failed(#condition, (message), ::std::source_location::current()); \
        }                                                                                             \
    } while (false)
// NOLINTEND(cppcoreguidelines-macro-usage)

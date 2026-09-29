// rr/core/result.hpp — Result<T> and the macros that propagate errors.
//
// Why this exists
//   `Result<T>` (an alias of std::expected<T, Error>) makes every expected
//   failure visible in function signatures (ADR-0007). The helpers keep call
//   sites short:
//
//     Result<Config> load(const Path& path) {
//         RR_TRY_ASSIGN(std::string text, read_file(path));   // declares `text`
//         RR_TRY(validate(text));                             // Result<void>
//         if (text.empty()) {
//             return rr::fail(Errc::invalid_argument, "config file is empty");
//         }
//         return parse(text);
//     }
//
//   The macros are plain statements (no GNU statement expressions), so they
//   stay portable to MSVC. They early-return the error unchanged; add context
//   with `.with_context()` where the extra information helps a reader.
//
// Thread-safety: header-only utilities with no state.
#pragma once

#include <expected>
#include <source_location>
#include <string>
#include <utility>

#include <rr/core/error.hpp>

namespace rr {

template <class T>
using Result = std::expected<T, Error>;

// True for Result<T> (any T). Lets a template insist that a callback returns a
// Result, with a readable static_assert instead of a wall of template errors.
template <class T>
inline constexpr bool is_result_v = false;
template <class T>
inline constexpr bool is_result_v<std::expected<T, Error>> = true;

// The error value to return from a function returning Result<T>.
// The source location defaults to the *caller* of fail().
[[nodiscard]] inline std::unexpected<Error> fail(
    Errc code, std::string message, std::source_location where = std::source_location::current()) {
    return std::unexpected<Error>(std::in_place, code, std::move(message), where);
}

// Re-wraps an existing error (e.g. after adding context) for returning.
[[nodiscard]] inline std::unexpected<Error> fail(Error error) {
    return std::unexpected<Error>(std::move(error));
}

// Success value for Result<void>, for symmetry with fail().
[[nodiscard]] inline Result<void> ok() noexcept {
    return {};
}

// Adds a context layer to a failed result and passes successes through.
//   return with_context(load_manifest(path), "while syncing models");
template <class T>
[[nodiscard]] Result<T> with_context(Result<T> result, std::string context) {
    if (!result.has_value()) {
        return std::unexpected(std::move(result).error().with_context(std::move(context)));
    }
    return result;
}

}  // namespace rr

// NOLINTBEGIN(cppcoreguidelines-macro-usage, bugprone-macro-parentheses)
// Macros because early return cannot be a function. `decl` is a declaration and
// `tmp` an identifier, so neither can be wrapped in parentheses.

#define RR_DETAIL_CONCAT_IMPL(a, b) a##b
#define RR_DETAIL_CONCAT(a, b) RR_DETAIL_CONCAT_IMPL(a, b)

// Evaluates `expr` (a Result<...>); on failure, returns its error from the
// enclosing function. The value, if any, is discarded.
//
// Each expansion names its temporary uniquely (__COUNTER__), so RR_TRY may
// wrap an expression whose lambda uses RR_TRY itself — the natural shape of
// `RR_TRY(db.write([&](WriteTx& tx) { RR_TRY(...); ... }))`. A fixed name
// would be shadowed inside the lambda, which -Wshadow rejects.
#define RR_TRY(expr) RR_DETAIL_TRY(RR_DETAIL_CONCAT(rr_try_result_, __COUNTER__), expr)

#define RR_DETAIL_TRY(tmp, expr)                                \
    do {                                                        \
        auto&& tmp = (expr);                                    \
        if (!tmp.has_value()) [[unlikely]] {                    \
            return ::std::unexpected(::std::move(tmp).error()); \
        }                                                       \
    } while (false)

// Evaluates `expr` (a Result<T>); on failure, returns its error; on success,
// initializes the declaration `decl` with the value:
//   RR_TRY_ASSIGN(auto rows, db.query(...));
//   RR_TRY_ASSIGN(existing_variable, compute());
#define RR_TRY_ASSIGN(decl, expr) \
    RR_DETAIL_TRY_ASSIGN(RR_DETAIL_CONCAT(rr_try_value_, __COUNTER__), decl, expr)

#define RR_DETAIL_TRY_ASSIGN(tmp, decl, expr)               \
    auto tmp = (expr);                                      \
    if (!tmp.has_value()) [[unlikely]] {                    \
        return ::std::unexpected(::std::move(tmp).error()); \
    }                                                       \
    decl = ::std::move(tmp).value()

// NOLINTEND(cppcoreguidelines-macro-usage, bugprone-macro-parentheses)

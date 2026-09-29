// rr/core/error.hpp — the error vocabulary of rReport.
//
// Why this exists
//   Most failures in this system are *expected*: timeouts, rate limits, invalid
//   model output, a pod that disappeared. They must be handled explicitly,
//   retried according to their class, and reported with enough context to act
//   on (ADR-0007). An Error is therefore a plain value:
//     * a class (Errc) that callers branch on — never on message text;
//     * a human-readable message;
//     * a context chain, appended while the error travels up the stack
//       ("while uploading blob …", "while running stage atomize");
//     * an optional retry hint (e.g. from an HTTP Retry-After header);
//     * the source location where it was created, for debugging.
//
// Thread-safety: Error is a regular value type with no shared state.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rr {

// Error classes. Each value names *what the caller should do*, which is why the
// retry policy (ADR-0007) can be written against this enum alone.
enum class Errc : std::uint8_t {
    invalid_argument,      // caller passed invalid input; fix the input, do not retry
    not_found,             // the requested entity does not exist
    conflict,              // state conflict, e.g. a duplicate key or a stale version
    permission_denied,     // authentication or authorization failed (HTTP 401/403)
    io,                    // local filesystem / operating-system failure
    network,               // could not reach a remote endpoint            (transient)
    timeout,               // an operation exceeded its deadline           (transient)
    unavailable,           // remote temporarily cannot serve (5xx, restarting engine) (transient)
    rate_limited,          // remote asked us to slow down; see retry_after (transient)
    cancelled,             // a stop was requested through a std::stop_token
    refused,               // a model refused or softened the task → fallback provider
    model_output_invalid,  // model output failed schema/validator checks → repair, then fallback
    budget_exceeded,       // a token/time/money budget ran out → stop and surface
    needs_user,            // a human must act (e.g. a YouTube bot check) → stop and surface
    unsupported,           // format or feature not supported by this build or peer
    integrity,             // data failed an integrity check (hash mismatch, corrupt file)
    internal,              // a bug: broken invariant or an unexpected exception
};

// Stable, lowercase name of an error class ("not_found"). Used in logs and APIs.
[[nodiscard]] std::string_view to_string(Errc code) noexcept;

// Parses the output of to_string(); nullopt for unknown names.
[[nodiscard]] std::optional<Errc> errc_from_string(std::string_view name) noexcept;

// True for error classes that are worth retrying with backoff.
[[nodiscard]] constexpr bool is_transient(Errc code) noexcept {
    return code == Errc::network || code == Errc::timeout || code == Errc::unavailable ||
           code == Errc::rate_limited;
}

class Error {
public:
    Error(Errc code, std::string message, std::source_location where = std::source_location::current());

    [[nodiscard]] Errc code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    // Context layers, innermost first (the order in which they were added).
    [[nodiscard]] std::span<const std::string> context() const noexcept { return context_; }

    [[nodiscard]] std::optional<std::chrono::milliseconds> retry_after() const noexcept {
        return retry_after_;
    }

    [[nodiscard]] const std::source_location& where() const noexcept { return where_; }
    [[nodiscard]] bool transient() const noexcept { return is_transient(code_); }

    // Appends a context layer. Rvalue-qualified so it chains on the way out:
    //   return std::unexpected(std::move(error).with_context("while syncing models"));
    [[nodiscard]] Error with_context(std::string context) &&;

    // Attaches a retry hint (e.g. parsed from a Retry-After header).
    [[nodiscard]] Error with_retry_after(std::chrono::milliseconds delay) &&;

    // One line for logs and CLI output:
    //   "timeout: no response after 30000 ms (while calling /health; while starting pod) [client.cpp:42]"
    [[nodiscard]] std::string describe() const;

    friend bool operator==(const Error& lhs, const Error& rhs) noexcept {
        return lhs.code_ == rhs.code_ && lhs.message_ == rhs.message_ && lhs.context_ == rhs.context_ &&
               lhs.retry_after_ == rhs.retry_after_;
    }

private:
    Errc code_;
    std::string message_;
    std::vector<std::string> context_;
    std::optional<std::chrono::milliseconds> retry_after_;
    std::source_location where_;
};

}  // namespace rr

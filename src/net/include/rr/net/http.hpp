// rr/net/http.hpp — HTTP vocabulary shared by the client and the server.
//
// Why this exists
//   The hub and the runtime talk HTTP to each other (ADR-0001), and the hub
//   also talks to model APIs. Both sides must agree on:
//     * how headers are compared (case-insensitively, per RFC 9110);
//     * how an rr::Error travels over HTTP. A structured JSON body keeps the
//       exact Errc across the wire:
//         {"error": {"code": "integrity", "message": "…", "retry_after_ms": 2000}}
//       A runtime failure then reaches the hub as the same error class (and
//       the same retry decision), not as a generic "HTTP 422";
//     * one table between Errc and status codes, for servers answering and
//       for clients reading bodies without our error format (other APIs);
//     * how a Retry-After header is read.
//
// Thread-safety: plain values and pure functions.
#pragma once

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rr/core/result.hpp>

namespace rr::net {

enum class Method : std::uint8_t {
    get,
    head,
    post,
    put,
    patch,
    del
};

[[nodiscard]] std::string_view to_string(Method method) noexcept;

// An ordered list of header fields. Names compare case-insensitively;
// duplicates are allowed (Set-Cookie, Link) and kept in order.
class Headers {
public:
    using Field = std::pair<std::string, std::string>;

    Headers() = default;
    Headers(std::initializer_list<Field> fields) : fields_(fields) {}

    // Appends a field, keeping any existing ones with the same name.
    void add(std::string name, std::string value);
    // Replaces every field with this name by a single one.
    void set(std::string name, std::string value);
    void remove(std::string_view name);

    // The first value with this name.
    [[nodiscard]] std::optional<std::string_view> get(std::string_view name) const;
    [[nodiscard]] std::vector<std::string_view> get_all(std::string_view name) const;
    [[nodiscard]] bool contains(std::string_view name) const { return get(name).has_value(); }

    [[nodiscard]] auto begin() const noexcept { return fields_.begin(); }
    [[nodiscard]] auto end() const noexcept { return fields_.end(); }
    [[nodiscard]] std::size_t size() const noexcept { return fields_.size(); }
    [[nodiscard]] bool empty() const noexcept { return fields_.empty(); }

private:
    std::vector<Field> fields_;
};

// The status a server answers with for an error class.
[[nodiscard]] int http_status_for(Errc code) noexcept;
// The error class for a status, when the body carries no structured error.
[[nodiscard]] Errc errc_for_http_status(int status) noexcept;

// The JSON body a server sends for an error (see the header comment).
[[nodiscard]] std::string error_body(const Error& error);

// The error a client reports for a non-2xx response: the structured error
// when the body has one, otherwise a class derived from the status, with the
// start of the body as detail. Retry-After is attached when present.
[[nodiscard]] Error error_from_response(int status, const Headers& headers, std::string_view body);

// The error carried by an SSE event's data, if it is an error event:
// {"error": {...}} in our structured shape (a string "code", decoded exactly)
// or in the llama.cpp/OpenAI shape, whose integer "code" is an HTTP status
// (decoded as error_from_response() would decode that status). A stream can
// only report errors this way once its own 200 is on the wire. std::nullopt
// for every other event, "[DONE]" included.
[[nodiscard]] std::optional<Error> error_from_event(std::string_view data);

// Parses a Retry-After value: delay-seconds ("120") or an IMF-fixdate
// ("Sun, 06 Nov 1994 08:49:37 GMT", relative to `now`). Past dates yield 0.
[[nodiscard]] std::optional<std::chrono::milliseconds> parse_retry_after(
    std::string_view value, std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

// Percent-encodes a URL path segment or query component (RFC 3986
// unreserved characters pass through; everything else becomes %XX).
[[nodiscard]] std::string url_encode(std::string_view component);

}  // namespace rr::net

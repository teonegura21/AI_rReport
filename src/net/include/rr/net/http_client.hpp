// rr/net/http_client.hpp — an HTTP(S) client on libcurl, without exceptions.
//
// Why this exists
//   The hub calls the runtime (blobs, jobs, streamed model calls) and model
//   APIs; the runtime proxies requests to its local engines. All of them go
//   through this client, which provides:
//
//   * Responses and failures kept apart. A transport failure (DNS, refused
//     connection, TLS, timeout, cancellation) is an Error; an HTTP status is
//     part of the Response. expect_success() turns non-2xx responses into
//     the right Error (see rr/net/http.hpp), including Retry-After hints.
//   * Streaming. stream() hands each piece of a 2xx body to a callback as it
//     arrives: SSE token streams, proxied responses. Error responses are
//     buffered instead, so they can be decoded into an Error.
//   * Timeouts that fit long streams: a total timeout, and an idle timeout
//     (no bytes for N ms) that a slow but steady token stream never trips.
//   * Instant cancellation. Every transfer runs on a curl "multi" handle,
//     whose poll is woken by curl_multi_wakeup() the moment the stop token
//     is triggered. (The easy API's progress callback only fires about once
//     a second when a transfer is idle.)
//   * Connection reuse. Handles are pooled and keep their connections alive;
//     DNS and TLS sessions are shared between handles.
//   * Safe defaults: TLS verification always on; http and https only (no
//     file://, no gopher://); no redirects unless asked; no
//     "Expect: 100-continue" round trip; transparent gzip/brotli/zstd; the
//     standard proxy environment variables are honoured.
//
// Thread-safety: HttpClient is thread-safe; any number of requests may run
//   concurrently. Stream callbacks run on the calling thread.
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

#include <rr/core/result.hpp>
#include <rr/net/http.hpp>

namespace rr::net {

class HttpClient {
public:
    struct Options {
        std::chrono::milliseconds connect_timeout{10'000};
        std::string user_agent = "rreport";
        // An extra certificate authority bundle (PEM). The system store is used otherwise.
        std::optional<std::filesystem::path> ca_bundle{};
        // Cap for buffered bodies (send(), and error bodies of stream()).
        std::size_t max_response_bytes = std::size_t{64} << 20U;
        // Idle handles (with their open connections) kept for reuse.
        std::size_t max_pooled_handles = 16;
    };

    struct Request {
        Method method = Method::get;
        std::string url{};
        Headers headers{};
        std::string body{};
        // Streams the body from this file instead of `body` (large uploads).
        std::optional<std::filesystem::path> body_file{};
        std::optional<std::chrono::milliseconds> timeout{};       // the whole exchange
        std::optional<std::chrono::milliseconds> idle_timeout{};  // no bytes for this long
        bool follow_redirects = false;                            // at most 5, http(s) only
    };

    struct Response {
        int status = 0;
        Headers headers{};
        std::string body{};  // empty for stream() when the status is 2xx

        [[nodiscard]] bool success() const noexcept { return status >= 200 && status < 300; }
    };

    struct StreamHandlers {
        // Called once with the status and headers, before any body bytes.
        std::function<Result<void>(int status, const Headers& headers)> on_head{};
        // Called with each piece of a 2xx body as it arrives. A failure aborts
        // the transfer and becomes the result.
        std::function<Result<void>(std::string_view chunk)> on_chunk{};
    };

    explicit HttpClient(Options options);
    HttpClient() : HttpClient(Options{}) {}
    HttpClient(HttpClient&&) noexcept;
    HttpClient& operator=(HttpClient&&) noexcept;
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    ~HttpClient();

    // Sends the request and buffers the whole response.
    [[nodiscard]] Result<Response> send(const Request& request, const std::stop_token& stop = {}) const;

    // Sends the request and delivers a 2xx body to `handlers` as it arrives.
    [[nodiscard]] Result<Response> stream(const Request& request, const StreamHandlers& handlers,
                                          const std::stop_token& stop = {}) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Passes 2xx responses through; turns any other status into its Error.
[[nodiscard]] Result<HttpClient::Response> expect_success(Result<HttpClient::Response> response);

}  // namespace rr::net

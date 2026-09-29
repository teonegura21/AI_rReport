// rr/net/http_server.hpp — a small HTTP server for rReport's own APIs.
//
// Why this exists
//   The runtime supervisor on the pod serves the hub (health, blobs, jobs,
//   model calls), and the hub will serve its web UI and MCP adapter. This
//   wrapper over cpp-httplib (kept out of the header) gives them one
//   well-behaved shape:
//
//   * Handlers are plain functions from a Request to a Response; errors are
//     answered with Response::error(), whose JSON body carries the exact
//     Errc (rr/net/http.hpp), so it reaches the hub intact.
//   * A guard runs before routing and before any body is read: the place
//     for authentication. A rejected client can never make the server
//     buffer a large upload.
//   * Buffered routes (JSON APIs) are limited to max_body_bytes, checked
//     against Content-Length before reading. Streaming routes (blob uploads)
//     read the body piece by piece through a BodyReader, up to
//     max_stream_bytes.
//   * Streamed responses (SSE, proxied model output) write through a
//     ChunkWriter, which reports when the client has gone away and when the
//     server is stopping, so producers can quit early.
//   * An exception escaping a handler becomes a 500 with a structured body,
//     not a dead process; a client that disconnects mid-stream produces a
//     failed write (MSG_NOSIGNAL), never a SIGPIPE.
//   * GET routes also answer HEAD (without sending the body).
//   * The port is exclusive: a second server on it fails to start, instead
//     of silently sharing it (see start()).
//   * TCP_NODELAY: small writes (streamed tokens) leave at once instead of
//     waiting on Nagle's algorithm for up to one delayed-ACK interval.
//
// Thread-safety: register routes and the guard before start(). Handlers run
//   concurrently on the server's worker threads.
#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/net/http.hpp>

namespace rr::net {

class HttpServer {
public:
    struct Options {
        std::string host = "127.0.0.1";
        int port = 0;  // 0: any free port (tests); see port()
        // httplib gives each connection a worker thread for its whole life,
        // idle keep-alive time included. `worker_threads` are always ready;
        // more are started on demand, up to `max_worker_threads` (the bound
        // on concurrent connections), and exit again when idle.
        std::size_t worker_threads = 8;
        std::size_t max_worker_threads = 128;
        std::size_t max_body_bytes = std::size_t{64} << 20U;   // buffered routes
        std::size_t max_stream_bytes = std::size_t{8} << 30U;  // streaming routes
        std::chrono::seconds read_timeout{30};
        std::chrono::seconds write_timeout{30};
        std::chrono::seconds keep_alive_timeout{5};
    };

    struct Request {
        Method method = Method::get;
        std::string path{};
        Headers headers{};
        std::string body{};                                          // buffered routes only
        std::unordered_map<std::string, std::string> path_params{};  // "/v1/blobs/:hash" → "hash"
        std::vector<std::pair<std::string, std::string>> query{};
        std::string remote_address{};

        [[nodiscard]] std::optional<std::string_view> path_param(std::string_view name) const;
        [[nodiscard]] std::optional<std::string_view> query_param(std::string_view name) const;
    };

    // Writes a streamed response body.
    class ChunkWriter {
    public:
        virtual ~ChunkWriter() = default;
        // Sends bytes now. False once the client is gone: stop producing.
        virtual bool write(std::string_view bytes) = 0;
        // True when the server is shutting down: finish up.
        [[nodiscard]] virtual bool stopping() const = 0;

    protected:
        ChunkWriter() = default;
        ChunkWriter(const ChunkWriter&) = default;
        ChunkWriter& operator=(const ChunkWriter&) = default;
        ChunkWriter(ChunkWriter&&) = default;
        ChunkWriter& operator=(ChunkWriter&&) = default;
    };

    // Reads a streaming route's request body.
    class BodyReader {
    public:
        virtual ~BodyReader() = default;
        // Calls `on_chunk` with each piece of the body, in order. Stops at the
        // first failure and returns it; Errc::io when the body cannot be read.
        [[nodiscard]] virtual Result<void> read(
            const std::function<Result<void>(std::string_view chunk)>& on_chunk) = 0;

    protected:
        BodyReader() = default;
        BodyReader(const BodyReader&) = default;
        BodyReader& operator=(const BodyReader&) = default;
        BodyReader(BodyReader&&) = default;
        BodyReader& operator=(BodyReader&&) = default;
    };

    struct Response {
        int status = 200;
        Headers headers{};
        std::string body{};
        std::string content_type = "text/plain; charset=utf-8";
        // When set, the body is produced by this function after the headers
        // are sent (chunked transfer encoding), and `body` is ignored.
        std::function<void(ChunkWriter& writer)> stream{};

        [[nodiscard]] static Response json(int status, std::string body);
        [[nodiscard]] static Response text(int status, std::string body);
        [[nodiscard]] static Response empty(int status);
        // The status for the error's class, and its structured JSON body.
        [[nodiscard]] static Response error(const Error& error);
        // A text/event-stream response (SSE), with caching disabled.
        [[nodiscard]] static Response event_stream(std::function<void(ChunkWriter& writer)> producer);
    };

    using Handler = std::function<Response(const Request& request)>;
    using StreamingHandler = std::function<Response(const Request& request, BodyReader& body)>;
    // Runs before routing and before the body is read. Returning a Response
    // answers the request with it (e.g. 401); std::nullopt lets it through.
    using Guard = std::function<std::optional<Response>(const Request& request)>;

    struct AccessLogEntry {
        std::string method;
        std::string path;
        int status = 0;
        std::string remote_address;
        std::chrono::milliseconds duration{0};
    };
    using AccessLog = std::function<void(const AccessLogEntry& entry)>;

    explicit HttpServer(Options options);
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    HttpServer(HttpServer&&) = delete;
    HttpServer& operator=(HttpServer&&) = delete;
    ~HttpServer();  // stops the server

    // Patterns use ":name" segments: "/v1/blobs/:library/:hash".
    void route(Method method, const std::string& pattern, Handler handler);
    // For POST/PUT/PATCH/DELETE whose bodies are too large to buffer.
    void route_streaming(Method method, const std::string& pattern, StreamingHandler handler);
    void set_guard(Guard guard);
    void set_access_log(AccessLog log);

    // Binds and starts serving on background threads. Errc::io when the
    // address cannot be bound (e.g. the port is in use).
    [[nodiscard]] Result<void> start();
    // The bound port (useful with port 0). 0 before start().
    [[nodiscard]] int port() const noexcept;
    // Stops accepting, lets running handlers finish, joins. Idempotent.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rr::net

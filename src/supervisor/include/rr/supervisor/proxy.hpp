// rr/supervisor/proxy.hpp — relaying a model call to a local engine, streaming.
//
// Why this exists
//   Model calls reach the pod as OpenAI-compatible requests and are served by
//   a llama-server on localhost (ADR-0003). The supervisor relays them, and
//   must stream: a generation can run for minutes, and the hub shows (and
//   checks) tokens as they arrive. The relay keeps these promises (ADR-0014):
//
//   * Back-pressure, not buffering. The engine's transfer runs on a thread of
//     its own and hands chunks over through a bounded buffer
//     (kMaxBufferedBytes). A slow client slows the engine's transfer down
//     (TCP back-pressure) instead of making the supervisor hold a whole
//     generation in memory.
//   * An engine's error keeps its meaning. An error status that arrives in
//     time is passed through whole. A streamed call whose engine has not
//     answered within `head_patience` is answered 200 by the relay itself
//     (see the next point); an error that comes after that travels in the
//     stream as one event, data: {"error": {...}}, carrying the same Errc and
//     retry hint the status would have given (rr/net/http.hpp).
//   * Silence is filled. RunPod's HTTP proxy (Cloudflare) closes a connection
//     that carries no byte for about 100 s, and an engine is silent while a
//     request waits for a slot and while it reads the prompt. A streamed call
//     therefore gets an SSE comment (": keep-alive") after each `keep_alive`
//     of downstream silence, inserted only where it cannot change an event
//     (net::SseLineTracker). A non-streamed call cannot be kept alive (its
//     status is unknown until the end); the hub streams its long calls.
//   * A broken answer looks broken. When the engine's transfer fails
//     mid-body (it crashed, or said nothing for `idle_timeout`), a streamed
//     call gets an error event where one fits, and the transfer is then cut
//     short (no final chunk): the hub sees Errc::network and retries, and
//     never takes a truncated answer for a complete one.
//   * Nobody works for a client that left. A client that goes away while the
//     relay waits for the engine (Request::client_gone) or while it relays (a
//     failed write or probe) cancels the engine's transfer at once;
//     llama-server notices the closed connection and frees the slot.
//   * Stopping is prompt. Every wait lasts at most one tick (kMaxTick). When
//     the runtime stops (`shutdown`), relays end with an honest error within
//     a tick instead of holding Supervisor::stop() for an engine's timeout.
//   * An InFlight guard is held for the whole exchange, so a long stream
//     keeps the pod's idle timer (Watchdog) from firing mid-generation.
//   * Only Content-Type and Accept are forwarded. The hub's bearer token is
//     the pod's secret, and llama-server acts on some headers (with
//     X-Conversation-Id it keeps generating after the client disconnects).
//
// Thread-safety: proxy() may be called concurrently; each call owns its state.
#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <stop_token>
#include <string>

#include <rr/net/http_client.hpp>
#include <rr/net/http_server.hpp>
#include <rr/supervisor/watchdog.hpp>

namespace rr::supervisor {

inline constexpr std::size_t kMaxBufferedBytes = std::size_t{4} << 20U;  // 4 MiB
// The longest any wait of the relay lasts before it re-checks the client, the
// shutdown request and the keep-alive timer.
inline constexpr std::chrono::milliseconds kMaxTick{100};

struct ProxyOptions {
    std::string url{};  // e.g. "http://127.0.0.1:18080/v1/chat/completions"
    // The engine's transfer fails after this long without a byte (reading a
    // long prompt on a busy GPU takes a while).
    std::chrono::milliseconds idle_timeout{std::chrono::minutes{10}};
    // Set for streamed calls (the request asked for "stream": true): the
    // longest the downstream connection may stay silent. std::nullopt: a
    // plain relay, which waits for the engine's status however long it takes.
    std::optional<std::chrono::milliseconds> keep_alive{};
    // Streamed calls: how long to wait for the engine's status before
    // answering 200 on its behalf.
    std::chrono::milliseconds head_patience{std::chrono::seconds{1}};
    // Requested when the runtime stops.
    std::stop_token shutdown{};
};

// Relays `request` (method, body, Content-Type, Accept) to `options.url` and
// returns the response to send (see the header comment for its promises).
[[nodiscard]] net::HttpServer::Response proxy(const net::HttpClient& client, ProxyOptions options,
                                              const net::HttpServer::Request& request,
                                              Watchdog::InFlight in_flight);

}  // namespace rr::supervisor

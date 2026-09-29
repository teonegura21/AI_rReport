// rr/net/sse.hpp — Server-Sent Events: parsing streams and formatting events.
//
// Why this exists
//   Every streamed model response arrives as an SSE stream: llama-server and
//   OpenAI-compatible APIs send `data: {json}` events ending with
//   `data: [DONE]`; Anthropic's API names each event (`event: message_delta`).
//   The runtime also streams job progress to the hub as SSE. Bytes arrive in
//   arbitrary chunks, so the parser is incremental. It follows the WHATWG
//   "interpreting an event stream" algorithm exactly:
//     * lines end with CRLF, LF or CR, even when a CRLF is split across two
//       network reads;
//     * `data:` lines accumulate (joined with '\n'); a blank line dispatches;
//       an event with no data is not dispatched;
//     * one space after the colon is removed; lines starting with ':' are
//       comments (servers send them as keep-alives);
//     * the last event ID is sticky across events; `retry:` accepts digits only;
//     * a UTF-8 byte-order mark at the very start is ignored;
//     * an unfinished event at the end of the stream is discarded.
//   A size cap protects against a peer that never ends a line or an event.
//
// Thread-safety: none; one parser per stream.
#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <rr/core/result.hpp>

namespace rr::net {

struct SseEvent {
    std::string type;  // the `event:` field, or "message" when absent
    std::string data;  // `data:` lines joined with '\n'
    std::string id;    // the last event ID in effect when this event was dispatched

    friend bool operator==(const SseEvent&, const SseEvent&) = default;
};

class SseParser {
public:
    using EventCallback = std::function<Result<void>(const SseEvent& event)>;

    static constexpr std::size_t kDefaultMaxEventBytes = std::size_t{16} << 20U;  // 16 MiB

    explicit SseParser(std::size_t max_event_bytes = kDefaultMaxEventBytes)
        : max_event_bytes_(max_event_bytes) {}

    // Parses `bytes` and calls `on_event` for every completed event. Stops at
    // the first failure: a failing callback's error, or Errc::invalid_argument
    // when a line or an event outgrows the size cap.
    [[nodiscard]] Result<void> feed(std::string_view bytes, const EventCallback& on_event);

    // The reconnection time most recently announced with `retry:`.
    [[nodiscard]] std::optional<std::chrono::milliseconds> retry() const noexcept { return retry_; }
    [[nodiscard]] const std::string& last_event_id() const noexcept { return last_event_id_; }

private:
    [[nodiscard]] Result<void> process_line(std::string_view line, const EventCallback& on_event);
    [[nodiscard]] Result<void> dispatch(const EventCallback& on_event);

    std::size_t max_event_bytes_;
    std::string line_;           // the current, unfinished line
    std::string data_;           // data buffer of the event being built
    std::string type_;           // event type buffer
    std::string last_event_id_;  // sticky across events (per the spec)
    std::optional<std::chrono::milliseconds> retry_;
    bool skip_line_feed_ = false;  // the previous chunk ended with CR: a leading LF belongs to it
    bool at_stream_start_ = true;  // a byte-order mark is only skipped here
};

// One event in wire format: optional `event:` and `id:` lines, one `data:`
// line per line of `data`, and the terminating blank line.
[[nodiscard]] std::string format_sse_event(std::string_view data, std::string_view type = {},
                                           std::string_view id = {});
// A comment line: ignored by parsers, useful as a keep-alive.
[[nodiscard]] std::string format_sse_comment(std::string_view text);

}  // namespace rr::net

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
// One comment line (": text\n"): ignored by parsers wherever a line may
// start, so it is a safe keep-alive (see SseLineTracker for where that is).
[[nodiscard]] std::string format_sse_comment(std::string_view text);

// Where a relay may insert its own lines into a stream of SSE bytes without
// changing the events a client parses.
//
// * A comment line (a keep-alive) fits at any line start: it is ignored
//   wherever a line may start, even between two lines of one event, since it
//   neither adds data nor ends the event.
// * A whole event (an error report) fits only between events: at a line
//   start with no field line since the last blank line. Otherwise a pending
//   "data:" or "event:" line would merge into the inserted event.
//
// Line starts need care with carriage returns. A CR may be the first half of
// a CRLF whose LF is still on its way, and anything inserted between the two
// turns that LF into a blank line, which ends the event early. So after a CR
// nothing fits until the next byte shows what the CR was (a stream that ends
// lines with bare CRs therefore gets no insertions: safe, if quiet). Parsers
// skip a byte-order mark only at the very start of a stream, so an insertion
// before the first relayed byte assumes there is none, as with llama-server
// and OpenAI-compatible servers.
//
// Thread-safety: none; one tracker per stream.
class SseLineTracker {
public:
    // Every byte sent downstream, in order: relayed bytes and insertions alike.
    void feed(std::string_view bytes) noexcept;

    [[nodiscard]] bool at_line_start() const noexcept { return !line_open_ && !after_cr_; }
    [[nodiscard]] bool between_events() const noexcept { return at_line_start() && !fields_pending_; }

private:
    bool line_open_ = false;        // bytes since the last line ending
    bool line_is_comment_ = false;  // the open line starts with ':'
    bool after_cr_ = false;         // the last byte was a CR: an LF next belongs to it
    bool fields_pending_ = false;   // a field line since the last blank line
};

}  // namespace rr::net

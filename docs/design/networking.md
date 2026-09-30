# rr_net: HTTP, streaming and retries, and the traps underneath

`rr_net` (`src/net/`) carries every byte between the hub, the GPU pod and model APIs. The code is small; what makes it correct is a set of details found by measurement and mutation testing. They are recorded here so nobody "simplifies" one away.

## Pieces

| Header | What it is |
|---|---|
| `rr/net/http.hpp` | Shared vocabulary: case-insensitive `Headers`, **one** `Errc` ↔ HTTP status table, the structured JSON error body, `Retry-After` parsing, URL encoding |
| `rr/net/http_client.hpp` | libcurl client: buffered `send()` and streaming `stream()`, total and idle timeouts, instant cancellation, pooled connections |
| `rr/net/http_server.hpp` | cpp-httplib wrapper: routes with `:params`, an auth guard, buffered and streaming bodies, streamed responses |
| `rr/net/sse.hpp` | Incremental Server-Sent Events parser (WHATWG algorithm), formatter, and `SseLineTracker` (where a relay may insert keep-alives) |
| `rr/net/retry.hpp` | `with_retries()`: full-jitter backoff, `Retry-After` wins, transient errors only, interruptible pauses |

## Errors cross the wire intact

A server answers an `Error` with a status from the table *and* a body:

```json
{"error": {"code": "integrity", "message": "hash mismatch", "context": ["while storing"], "retry_after_ms": 2000}}
```

`error_from_response()` restores the exact `Errc`, message, context and retry hint.

**Why this matters:** status codes alone are lossy. `io` and `internal` are both 500, and `refused` and `integrity` are both 422. The retry decision depends on the class, so without the body a runtime failure would be retried (or not) for the wrong reason.

For other APIs (OpenAI- or Anthropic-style bodies) the status decides the class, and their `message` is kept. All 5xx map to `unavailable`, which is transient: model APIs answer 500/502/503/529 for overload and recommend retrying.

**Inside a stream.** Once a stream's `200` is on the wire, an error can only travel as an event: `data: {"error": {...}}`. `error_from_event()` decodes both our shape (string `code`, exact) and the llama.cpp/OpenAI shape, whose integer `code` is the HTTP status the error stands for. That status is then decoded as `error_from_response()` would decode it: a 400 stays `invalid_argument` and is not retried, and a 500 becomes `unavailable` and is.

## A stream's three honest endings

A client reading a streamed body must be able to tell three cases apart. The server side of `rr_net` makes each look different on the wire:

| How the body ends | On the wire | What our client returns |
|---|---|---|
| Complete | the final zero-length chunk | the response |
| Failed after the status was sent | an error event (when the producer can send one), then the connection closes **without** the final chunk | `Errc::network` (curl: `CURLE_PARTIAL_FILE`), which is retried |
| Cancelled by the client | nothing more: the client closed the connection | `Errc::cancelled` |

**The mechanism.** A stream producer returns `Result<void>`.
- On `ok()`, the server writes the final chunk.
- On an error, or an exception, it returns `false` to cpp-httplib *without* calling `done()`. httplib then closes the connection before the terminator (verified in 0.58.0's `write_content_chunked`).

**Why this matters.** Before this change, a producer that gave up still ended the body cleanly, so a model answer cut off by an engine crash looked complete to the hub. Tests: `AFailingProducerTruncatesTheBody` covers both a returned error and a thrown exception.

## Heads, liveness and keep-alives

- **The head arrives with the header block, not with the first body byte.**
  - `on_head` fires at the blank line that ends the final header block. Interim `1xx` responses, and redirects curl is about to follow, are skipped. An HTTPS proxy's `CONNECT` reply is suppressed (`CURLOPT_SUPPRESS_CONNECT_HEADERS`).
  - Why: llama-server answers `200` as soon as a request gets a slot, then reads the prompt for as long as it takes. Before this change the client learned of the `200` only with the first token.
  - Test: `TheHeadArrivesBeforeTheFirstBodyByte`. Its server withholds the body until the client has seen the head.
- **Handlers can see their client leave.**
  - `Request::client_gone()` peeks at the connection's socket (httplib's `is_connection_closed`).
  - A handler that waits for something slow, such as the relay waiting for an engine's head, polls it so it can give up for a client that is gone.
  - It is valid only while the handler runs.
  - Test: `HandlersCanSeeTheirClientLeave`.
- **Liveness probe.** An empty `ChunkWriter::write({})` sends nothing and reports whether the client is still there. A waiting producer can probe without writing.
- **A relay inserts its own lines only where they cannot change an event.** `SseLineTracker` sees every byte sent downstream, including the relay's own, and answers two questions:
  - **Where a comment fits (a keep-alive).** At any line start. `format_sse_comment()` writes one comment line with no blank line after it, and a comment line is ignored wherever a line may start, even between two lines of one event. A blank line would end an event early.
  - **Where a whole event fits (an error report).** Only between events: at a line start with no field line since the last blank line. Otherwise a pending `data:` or `event:` line would merge into it.
  - **Carriage returns.** A CR may be half of a CRLF, and anything inserted between the CR and the LF turns the LF into a blank line. So nothing fits after a CR until the next byte arrives.
  - **Tests.**
    - `CommentsWhereverItAllowsNeverChangeTheEvents` and `EventsWhereverItAllowsLeaveTheOthersIntact` insert at every position the tracker allows, end included, in five streams (LF, CRLF, mixed, multi-line, commented).
    - With comments, the parsed events must be unchanged. With events, each inserted event must arrive on its own and every original event unchanged.
    - `ACommentInsideACrlfWouldSplitTheEvent` shows the trap the CR rule avoids.
- **Byte-exact downloads.** `Request::accept_compression = false` sends no `Accept-Encoding`, so the server sends the stored bytes and byte ranges refer to the stored offsets. Model downloads resume with `Range`, and some CDNs ignore `Range` on encoded responses.

## Client: why a multi handle per request

- **The multi handle.** Each request runs its easy handle through its own `curl_multi` handle, even though it is a blocking call.
- **Why not the easy API.** The easy API notices a stop only in its progress callback, which fires about once per second on an idle transfer.
- **The wake-up.** A `std::stop_callback` calls `curl_multi_wakeup()`, which interrupts `curl_multi_poll()` at once. The "stop generating" button therefore takes effect immediately, even when the stream is silent.
- **Test.** `CancellationWakesEvenASilentTransfer` fails when the wake-up is removed.

Other client rules:
- **Errors vs statuses.** Transport failures are errors; HTTP statuses are data. `expect_success()` converts non-2xx.
- **Streaming callbacks see only 2xx bodies.** Error bodies are buffered, so they can become an `Error`.
- **Idle timeout vs total timeout.** A 10-minute token stream is fine; 60 s of silence is not.
- **No exception may unwind through libcurl's C frames.** Callbacks run inside `guarded()`.
- **`curl_easy_setopt` is variadic.** Numeric options must be `long` or `curl_off_t`; `OptionSetter` rejects `int` at compile time.
- **Safe defaults.** TLS verification always on; http(s) only; no redirects unless asked; no `Expect: 100-continue`; `TCP_NODELAY` (curl's default).

## Server: four things cpp-httplib does by default that we must undo

| httplib default | Consequence | What we do | Test that pins it |
|---|---|---|---|
| **Elastic pool grows only when zero workers are idle** at enqueue time | Connections arriving together are all counted against the same idle workers; the surplus waits until a keep-alive connection times out. Measured: 9 simultaneous connections against 8 workers stalled one for exactly 5,105 ms, every time | `ElasticTaskQueue` grows whenever *queued jobs exceed idle workers*; extra workers exit after 30 s idle and are joined, never detached | `SimultaneousConnectionsNeverWaitForKeepAliveTimeouts` |
| **`SO_REUSEPORT`** on the listening socket | A second process binds the same port and the kernel splits connections between them: silent split-brain | `SO_REUSEADDR` only: restarts still work, a second live listener fails | `APortInUseIsReported` |
| **`send()` flags 0**, and the `Server` constructor sets **SIGPIPE to `SIG_IGN` process-wide** | Without the ignore, a client disconnecting mid-stream kills the process (SIGPIPE, exit 141). With it, every child process inherits an ignored SIGPIPE | Compile with `CPPHTTPLIB_SEND_FLAGS=MSG_NOSIGNAL` (every `rr_net` TU); `rr_proc` resets signal dispositions for children (ADR-0013) | `CancellingAStreamIsInstantAndTheServerNotices` (runs with SIGPIPE at `SIG_DFL`) |
| **`TCP_NODELAY` off** | Nagle's algorithm can hold small writes (streamed tokens) until the previous one is ACKed: tens of ms per chunk with delayed ACKs | `set_tcp_nodelay(true)` | — (a latency property; not observable deterministically on loopback) |

Keep in mind: httplib dedicates **one worker thread per connection for the connection's whole life**, idle keep-alive time included. `max_worker_threads` therefore bounds concurrent connections, not concurrent requests.

## Server: limits and the guard

- **The guard runs before any body is read.** It runs before routing (httplib's pre-routing hook), so an unauthenticated client cannot make the server buffer an upload.
- **Buffered routes.** They are held to `max_body_bytes` by checking `Content-Length` after routing and before reading. Chunked bodies are refused with 411: a buffered route must know the size up front.
- **Streaming routes.** They read through `BodyReader` up to `max_stream_bytes`, httplib's global payload limit.
- **Exceptions.** A handler exception becomes a 500 with a structured body. A stream producer's exception truncates the body; see "A stream's three honest endings".
- **Stopping.** Streamed responses check `ChunkWriter::stopping()` and a failed `write()`, so producers end when the server stops or the client leaves.

## SSE parsing details that matter

- **Line endings.** CR, LF and CRLF all end a line, including a CRLF split across two network reads.
- **Byte-order mark.** Only a *leading* BOM is skipped. Later on it is an ordinary character (it spoils that line's field name, per the spec).
- **Dispatch.** Events without data are not dispatched; an unfinished event at end of stream is discarded; the last event ID is sticky; `retry:` takes digits only.
- **Size cap.** It protects against a peer that never ends a line or an event.
- **Chunking.** The test suite feeds a complex stream split at *every* byte offset, and byte by byte, and requires identical events.

## How the tests were validated

Every mechanism above was mutation-tested: removed on purpose, and the named test was confirmed to fail.

| Mechanism removed | Caught by |
|---|---|
| Elastic queue (back to httplib's pool) | `SimultaneousConnectionsNeverWaitForKeepAliveTimeouts` |
| `SO_REUSEADDR`-only socket option | `APortInUseIsReported` |
| `MSG_NOSIGNAL` | `CancellingAStreamIsInstantAndTheServerNotices` (process died, exit 141) |
| `curl_multi_wakeup` on stop | `CancellationWakesEvenASilentTransfer` |
| Idle timeout | `AnIdleStreamTimesOut` |
| Buffering error bodies of streams | `ErrorBodiesOfStreamsAreBufferedNotStreamed` |
| Exception guard around callbacks | `HandlerFailuresAbortTheStream` |
| Content-Length check on buffered routes | `BufferedRoutesRefuseOversizedBodiesBeforeReadingThem` |
| Guard before routing | `TheGuardRejectsBeforeAnyHandlerRuns` |
| CRLF split across chunks | `AcceptsEveryLineEndingEvenSplitAcrossChunks` |
| Structured error codes | `CarryEveryErrorClassAcrossTheWireExactly` |
| Truncating a failed stream (`done()` called anyway) | `AFailingProducerTruncatesTheBody` |
| Head at the end of the header block (back to first body byte) | `TheHeadArrivesBeforeTheFirstBodyByte` |
| Skipping followed redirects' heads | `TheHeadIsTheFinalResponses` |
| `Request::client_gone` | `HandlersCanSeeTheirClientLeave` |
| Opting out of compression | `CompressionCanBeDeclinedForByteExactDownloads` |
| No line start after a CR (CR accepted) | `CommentsWhereverItAllowsNeverChangeTheEvents` |
| One-line comments (blank line appended) | `CommentsWhereverItAllowsNeverChangeTheEvents`, `RoundTripsThroughTheParser` |
| Pending fields reset by a blank line | `EventsWhereverItAllowsLeaveTheOthersIntact` |
| Comments leave no field pending | `AnEventFitsOnlyBetweenEvents` |
| A CRLF counted as one line ending | `EventsWhereverItAllowsLeaveTheOthersIntact` |
| Events only between events (any line start) | `EventsWhereverItAllowsLeaveTheOthersIntact` |
| Integer status codes in error events | `DecodeOurShapeAndLlamaCpps` |

Mutants are applied by pattern, and a pattern must match exactly once. An ambiguous one once mutated `format_sse_event()` instead of `format_sse_comment()`, and the mutant "survived" for the wrong reason.

The whole suite also passed 10 consecutive runs under ThreadSanitizer.

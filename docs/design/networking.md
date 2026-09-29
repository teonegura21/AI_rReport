# rr_net: HTTP, streaming and retries, and the traps underneath

`rr_net` (`src/net/`) carries every byte between the hub, the GPU pod and model APIs. The code is small; what makes it correct is a set of details found by measurement and mutation testing. They are recorded here so nobody "simplifies" one away.

## Pieces

| Header | What it is |
|---|---|
| `rr/net/http.hpp` | Shared vocabulary: case-insensitive `Headers`, **one** `Errc` ↔ HTTP status table, the structured JSON error body, `Retry-After` parsing, URL encoding |
| `rr/net/http_client.hpp` | libcurl client: buffered `send()` and streaming `stream()`, total and idle timeouts, instant cancellation, pooled connections |
| `rr/net/http_server.hpp` | cpp-httplib wrapper: routes with `:params`, an auth guard, buffered and streaming bodies, streamed responses |
| `rr/net/sse.hpp` | Incremental Server-Sent Events parser (WHATWG algorithm) and formatter |
| `rr/net/retry.hpp` | `with_retries()`: full-jitter backoff, `Retry-After` wins, transient errors only, interruptible pauses |

## Errors cross the wire intact

A server answers an `Error` with a status from the table *and* a body:

```json
{"error": {"code": "integrity", "message": "hash mismatch", "context": ["while storing"], "retry_after_ms": 2000}}
```

`error_from_response()` restores the exact `Errc`, message, context and retry hint.

**Why this matters:** status codes alone are lossy. `io` and `internal` are both 500, and `refused` and `integrity` are both 422. The retry decision depends on the class, so without the body a runtime failure would be retried (or not) for the wrong reason.

For other APIs (OpenAI- or Anthropic-style bodies) the status decides the class, and their `message` is kept. All 5xx map to `unavailable`, which is transient: model APIs answer 500/502/503/529 for overload and recommend retrying.

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
- **Exceptions.** A handler exception becomes a 500 with a structured body.
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

The whole suite also passed 10 consecutive runs under ThreadSanitizer.

# ADR-0014: The pod's relay keeps streams alive and never disguises a failure

- Status: Accepted
- Date: 2026-09-30

## Context

The runtime on the GPU pod relays OpenAI-compatible model calls from the hub to a llama-server on localhost (ADR-0003). Three facts about the path shape how the relay must behave.

1. **RunPod's HTTP proxy closes silent connections.**
   - Pods are reached through Cloudflare (`https://<pod>-<port>.proxy.runpod.net`), which closes a connection when about 100 s pass without a byte from the origin (a 524 before the headers, a cut connection after them).
   - The limit is on silence, not on total duration. Any byte resets it.
2. **Engines are silent for long stretches.** A request can wait for a free slot, then the model reads its prompt, and nothing is sent in either phase.
   - llama-server answers a streamed request with its `200` once the request gets a slot (current source; older builds only with the first token), then emits `:` pings every 30 s by default.
   - Nothing at all is sent while the request waits in the queue.
   - A long prompt on a 27B model can take a minute or more to read.
3. **An engine can fail mid-answer.** It can crash, run out of memory or be killed.
   - Before this decision, the relay, like `rr_net`'s server, ended the body cleanly when its upstream broke off.
   - A truncated answer therefore looked complete to the hub. That is exactly the silent loss the product exists to prevent: a Retold Article built on an answer cut in half.

## Decision

The relay (`src/supervisor/proxy.cpp`) makes six promises, and the hub may rely on them.

1. **Streamed calls are answered in time.** For a request with `"stream": true`, the relay waits for the engine's status for at most `stream_head_patience` (1 s).
   - An error status that arrives in time is passed through whole.
   - Otherwise the relay answers `200 text/event-stream` itself.
2. **Silence is filled.** In a streamed call, after each `stream_keep_alive` (15 s) of downstream silence, the relay sends an SSE comment line (`: keep-alive`).
   - It does so only where the comment cannot change an event: at a line start, never between the CR and LF of a CRLF (`net::SseLineTracker`).
   - A comment line is ignored by every conforming parser wherever a line may start.
3. **Errors after the 200 travel in the stream.** An engine error that arrives after the relay's own `200` becomes one event, `data: {"error": {...}}`, in `rr_net`'s structured shape.
   - It carries the same `Errc` and retry hint the status would have given: a late 400 is still `invalid_argument`, and is not retried.
   - An event is inserted only between events.
4. **A broken answer looks broken.** When the engine's transfer fails after part of its answer went out:
   - the relay sends an error event if one fits;
   - it then cuts the transfer short, closing the connection without the final chunk (`rr_net` producers return `Result<void>`);
   - the hub sees `Errc::network` and retries. If nothing of an answer went out, the error event is the whole body and the stream ends cleanly.
5. **Nobody works for a client that left.** A client that disconnects is noticed while the relay waits for the engine (`Request::client_gone`), and during silences (an empty write probes the connection). The relay then cancels the engine's transfer at once, and llama-server frees the slot.
6. **Stopping is prompt, and headers stay at the door.**
   - Every wait in the relay lasts at most one tick (100 ms). When the runtime stops, relays end with `unavailable` within a tick, instead of holding `Supervisor::stop()` for an engine's timeout.
   - Only `Content-Type` and `Accept` are forwarded to the engine. The bearer token is the pod's secret, and llama-server acts on some headers (`X-Conversation-Id` keeps it generating after a disconnect).

**The hub's side of the contract** (implemented in `rr_llm`, M0 Task 8):
- a streamed call succeeded only if `[DONE]` arrived;
- an error event decides the error's class (`net::error_from_event()` reads our shape and llama.cpp's);
- a transfer that ends without `[DONE]` and without an error event is `Errc::network`, and is retried.

Non-streamed calls cannot be kept alive, because their status is unknown until the end. The hub streams every long call.

## Consequences

- A call survives any silence, whether the engine is queued or reading a prompt, behind RunPod's proxy. This still has to be verified on a real pod (M0 acceptance).
- The hub never mistakes a truncated answer for a complete one. The hub needs no special handling for proxy timeouts; they cannot happen.
- A streamed call's HTTP status is `200` even when the engine later refuses it. The refusal's class is exact, but it is read from the stream, not the status line. Clients other than rReport's hub must read error events; llama-server's own streams already require this.
- The relay has more states than a byte pump. Each promise is pinned by an end-to-end test against `rr_fake_engine`. Each was removed on purpose and its test failed (the table in `docs/design/runtime.md`).

## Alternatives considered

- **Expose the pod's port over TCP instead of the HTTP proxy.**
  - No 100 s limit, but no TLS, and a random external port.
  - We would have to terminate TLS on the pod and manage certificates for ephemeral pods. The proxy's TLS is worth keeping.
- **Rely on llama-server's own pings (`--sse-ping-interval`).**
  - They start only after the `200`, which is sent when a slot is free, so a queued request stays silent.
  - Older builds reject the flag, and the pinned engine version decides which behavior we get. The relay must not depend on either.
- **Answer every streamed call with `200` at once.** This would lose the status of errors that are known immediately (a prompt over the context size). One second of patience keeps them exact.
- **End a broken stream cleanly after an error event.** Correct for rReport's hub, which reads error events, but any client that does not would take a truncated answer for a complete one. Cutting the transfer short is honest at the HTTP level too.

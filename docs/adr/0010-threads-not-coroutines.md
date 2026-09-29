# ADR-0010: Threads and stop tokens, not coroutines

- Status: Accepted
- Date: 2026-09-29

## Context

The hub runs many concurrent network calls (LLM missions, uploads), CPU work (alignment, checks) and subprocesses. Choices include asio + C++20 coroutines, a custom executor, or plain threads.

## Decision

- **Plain threads.** `std::jthread` + `std::stop_token` everywhere. Blocking I/O runs on per-resource-class thread pools: hub CPU = cores/2 (keeps the laptop usable), network I/O = 16 threads.
- **Limiters.** A `Limiter` per model endpoint, sized to its parallel slots, provides backpressure and priorities.
- **Cancellation.** It flows through `std::stop_token`:
  - `std::stop_callback` kills subprocesses;
  - the libcurl progress callback aborts transfers;
  - waits use `std::condition_variable_any` with a stop token.
- **One analysis at a time in v1**, with its missions in parallel.

## Consequences

- The concurrency model is easy to read, debug and sanitize (TSan works out of the box).
- Throughput is bounded by thread counts, which is plenty for one laptop or one tenant shard.
- Revisit for the hosted hub if profiling shows thread pressure. Async I/O can then be introduced behind the same interfaces.

## Alternatives considered

- **asio + coroutines now.** More scalable, but a steep complexity cost we don't need yet. Rejected for v1.

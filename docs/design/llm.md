# Calling models: `rr_llm`

Every model call in rReport goes through `rr_llm`: the infantry's missions, the commander's passes, the benchmark. This note explains how a call is built, streamed, checked, retried, accounted for and replayed, and why each rule exists.

## Pieces

| Where | What |
|---|---|
| `src/llm/include/rr/llm/chat.hpp` | The vocabulary: messages (text and image parts), `ChatRequest`, `ChatResponse`, `StreamSink`, `request_key()` |
| `src/llm/include/rr/llm/provider.hpp` | `LlmProvider` (one attempt at a call) and `RetryingProvider` |
| `src/llm/include/rr/llm/openai_provider.hpp` | The OpenAI-compatible streaming provider: llama-server behind the runtime (every model call goes there, ADR-0017) |
| `src/llm/include/rr/llm/accounting.hpp` | `AccountedProvider`: every attempt becomes a `CallRecord` |
| `src/llm/include/rr/llm/replay.hpp` | `RecordingProvider` and `ReplayProvider`: answers recorded once, replayed without a model |
| `src/llm/include/rr/llm/structured.hpp` | `structured<T>()`: answers that satisfy a JSON schema and the caller's checks |
| `src/library/include/rr/library/call_log.hpp` | `CallLog`: the records, kept in the library database |

## A call, as it is wired

Providers wrap providers, so each concern is written once and works with every backend:

```
structured<T>()                     schema, checks, one repair
  └ RetryingProvider                transient failures, with backoff; the sink hears on_restart()
      └ AccountedProvider           one record per attempt (the library's llm_calls)
          └ OpenAiProvider          one streamed attempt over HTTP
```

In record mode a `RecordingProvider` sits above the live provider. In replay mode a `ReplayProvider` replaces the live one, and never calls a model.

The vocabulary says *what* is asked, never how a server spells it:

- **`thinking` is a request, not a template flag.** llama-server spells it `chat_template_kwargs.enable_thinking`. To switch thinking off, the provider also sends `reasoning_effort: "none"`, which some templates honour instead.
- **`json_schema` is the model-facing schema** (`json::to_model_facing()`); validation always uses the full one.
- **The tag says why a call was made** (purpose, job, mission). It is recorded, never sent, and not part of the call's identity.
- **`extra` carries server-specific fields** (`cache_prompt`, `ignore_eos`, `id_slot`). A key the provider sets itself is refused rather than silently overwritten.

## Streaming, honestly

Every call streams (plan §4.1): RunPod's proxy cuts connections that stay silent too long, and only a stream can say "still working" (the runtime's keep-alives, ADR-0014). The provider reads a stream as strictly as the relay writes it:

1. **A call succeeded only if `[DONE]` arrived.** A stream that ends without it, or is cut short, is `Errc::network`. That class is transient, so the call is tried again: a partial answer is never trusted.
2. **An error event is the call's error.** `data: {"error": {…}}` becomes the error, in the exact class the engine or the runtime gave it (`net::error_from_event()`):
   - llama-server's integer `code` is read as an HTTP status;
   - the runtime's string `code` is an `Errc` name, kept exactly.
3. **Comment lines are ignored.** They are keep-alives. The idle timeout (60 s) is longer than the runtime's keep-alive interval (15 s), so only a call that has really gone silent trips it.
4. **A 2xx answer that is not `text/event-stream` is refused** (`unsupported`), rather than waited out.
5. **Cancellation is instant.** The stop token interrupts the transfer (`rr_net`'s multi-handle wake-up).

### What llama-server sends

These facts come from llama.cpp's source at `db00347` (2026-09-30), and the fake engine reproduces them:

- **The role chunk.** The first chunk's delta is `{"role": "assistant", "content": null}`, sent with the first token: the prompt is read in silence.
- **Deltas.** Thinking arrives as `delta.reasoning_content` (the default `--reasoning-format` is `deepseek`), the answer as `delta.content`. Keys absent from a delta are missing, not null.
- **The finish chunk** has `"delta": {}` and `finish_reason`: `stop`, `length` or `tool_calls`.
- **Usage** is sent only when `stream_options.include_usage` asks for it: a last chunk with `"choices": []`, whose `prompt_tokens` counts cached tokens too (`prompt_tokens_details.cached_tokens`).
- **`timings`** ride on the last JSON chunk: `prompt_per_second`, `predicted_per_second` and the rest. The engine's own speeds are what `bench` reports.
- **Errors.** Before the stream, an error is a plain JSON error with its status: 400 for a prompt longer than the context, 503 while loading. Mid-answer it is one error event, then the end, with no `[DONE]`.
- **Busy slots queue.** A queued streamed request gets no headers until a slot frees up. That silence is why the runtime answers `200` early and fills it with keep-alives.

### What the provider sends

```json
{"model": "commander", "messages": [...], "stream": true, "stream_options": {"include_usage": true},
 "max_tokens": 512, "temperature": 0.2, "seed": 42,
 "response_format": {"type": "json_schema", "json_schema": {"name": "atom", "schema": {...}, "strict": true}},
 "chat_template_kwargs": {"enable_thinking": false}, "reasoning_effort": "none"}
```

- **The schema goes in OpenAI's shape**, the one llama-server reads it from. llama.cpp's README also shows `{"type": "json_schema", "schema": …}`, but the server's code ignores that `schema`.
- **The schema never reaches the prompt,** so prompts must describe the format themselves.
- **A message with one text part is sent as a plain string.** Every compatible server takes that form. Otherwise the content is an array of parts, with images as `image_url`.
- **The dialect decides the two extensions.** `llama_server` switches thinking and reads `timings`. `openai` refuses a request that sets `thinking`, because no standard field carries it.

## Answers that satisfy a schema

Missions ask for data, not prose, and nothing uses an answer before it is validated (plan §8). `structured<T>()`:

1. **Sends the model-facing schema** where the provider supports constrained decoding, and switches thinking off unless the request says otherwise. A reasoning model under a grammar can spend its budget thinking, and some parsers constrain the thinking too (llama.cpp issue #20345).
2. **Parses and validates the answer.** A Markdown fence around the JSON is tolerated. The answer is checked against the *full* schema, then by the caller's own checks (what a schema cannot say), then converted to `T`. A conversion error is one more problem.
3. **Sends an unusable answer back once** (ADR-0007: one repair, then a fallback model). The model's own answer goes back, followed by a user turn listing every problem. An answer cut off at `max_tokens` is called out as such, since the only fix is a shorter answer.
4. **Fails with `model_output_invalid` if it is still unusable,** listing every problem, so the caller can escalate to another model.

Each attempt is an ordinary call: accounted for, retried and replayable like any other.

## Record and replay

Pipeline tests (layer L3) run whole videos without a GPU:

- **Keys.** A recording is `<directory>/<request key>.json`. The key is BLAKE3 over the canonical JSON of what the model sees, prefixed with the provider's name. Unset fields are left out of that JSON, so adding a field to `ChatRequest` later changes no existing key.
- **Files.** A recording keeps the request, pretty-printed, so people can read it and review its diff. It is written atomically (`write_file_atomically()`).
- **What is recorded.** Only successes. A replayed transient failure would make a test depend on luck.
- **Replay is strict.** A request without a recording fails with `not_found` and names its key. A recording copied under another call's name is refused as `integrity`.

## Accounting

Tokens and GPU time are the product's cost (plan §4.3). `AccountedProvider` hands one `CallRecord` per attempt to a `CallRecorder`. `rr_library`'s `CallLog` keeps records in the library database (`migrations/0001_accounting.sql`):

- **`llm_calls`** holds one row per attempt: start, duration, time to the first token, provider, model, tag, request key, status (`ok` or the error class), finish reason and tokens.
  - Attempts of one call share a request key, so a flaky pod shows up as several rows, not one slow call.
  - A CHECK ties `status` and `error` together.
- **`llm_call_payloads`** holds what was sent and received, apart from the numbers, so `prune_payloads()` can apply a retention policy without touching the accounting.
- **Recording never fails a call.** An answer the model already produced, and the GPU already paid for, is not thrown away because a row could not be written. The failure is logged.

`rr_llm` knows nothing of SQLite: the library implements the recorder interface. This keeps `rr_llm`'s dependencies as the plan's §7 table has them.

## How it is tested

| Level | What |
|---|---|
| Unit, scripted provider | `tests/unit/llm/scripted_provider.hpp` plays answers and errors, and keeps every request it was given. It covers retries, restarts, accounting, record/replay and `structured<T>()`, repair conversation included |
| Unit, scripted server | `openai_provider_test.cpp` serves exact bytes: llama-server's stream shape, keep-alive comments, a missing `[DONE]`, error events and statuses, silence and cancellation |
| End to end | `runtime_client_test.cpp` calls through the real runtime's relay to fake engines. Tokens, reasoning, usage and timings arrive intact; a 400 stays a bad request; a crash mid-answer becomes a retried network error; an engine's error event keeps its class |
| The library | `call_log_test.cpp` round-trips every field, NULLs included; covers filters, newest first, pruning, and a duplicate refused without a trace |

### How the tests were validated

Each rule was removed on purpose, and the named test was confirmed to fail. (Mutants are applied by pattern, a pattern must match exactly once, and the source is restored after each run.)

| Rule removed | Caught by |
|---|---|
| A call succeeded only if `[DONE]` arrived | `AnAnswerWithoutDoneIsIncomplete` |
| An error event is the call's error | `AnErrorEventIsTheCallsError` |
| Usage is asked for (`include_usage`) | `SendsWhatLlamaServerReads` |
| The schema goes where llama-server reads it | `SendsWhatLlamaServerReads` |
| A 2xx answer must be `text/event-stream` | `ErrorStatusesKeepTheirClass` |
| `extra` cannot override the provider's own keys | `RefusesWhatItCannotCarry` |
| `on_restart()` before a retry | `TriesTransientFailuresAgainAndRestartsTheStream` |
| Failures are accounted for | `RecordsFailuresToo` |
| A failure to record never fails the call | `AnAnswerOutlivesAFailureToRecordIt` |
| A recording must carry the call's key | `RefusesFilesThatAreNotThisCallsRecording` |
| The provider's name is part of the key | `TheRequestKeyIsWhatTheModelSees` |
| The tag is not part of the key | `TheRequestKeyIsWhatTheModelSees` |
| Unset fields stay out of the JSON, and so out of the key | `TheJsonFormLeavesUnsetFieldsOut` |
| One repair | `SendsAnInvalidAnswerBackWithItsProblems` |
| Validation against the full schema, even under a grammar | `SendsAnInvalidAnswerBackWithItsProblems` |
| Thinking off for structured calls | `AcceptsAValidAnswer` |
| A Markdown fence is tolerated | `LeavesWhatTheProviderCannotDoAlone` |
| A cut-off answer is called out | `SaysWhenAnAnswerWasCutShort` |
| Payloads are kept (`llm_call_payloads`) | `KeepsEveryField` |
| Pruning keeps the accounting | `PruningDropsOldPayloadsAndKeepsTheAccounting` |
| A missing job is NULL, not `''` | `KeepsFailuresAndMissingValues` |
| Newest first | `ListsNewestFirstAndFilters` |

**A mutant that crashed instead of failing.** Moving the schema out of `json_schema` first made the test program abort, taking every later test with it. The test read the request body through a `const` JSON value, whose `operator[]` asserts on a missing key. It now reads through a non-`const` one, whose `operator[]` yields null, so the expectation fails and names the difference.

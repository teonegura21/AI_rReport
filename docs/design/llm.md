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
| `src/llm/include/rr/llm/sampling.hpp` | `SamplingPreset` and `apply()`; the checks on every request's values; the rule against penalties |
| `src/llm/include/rr/llm/memo.hpp` | `MemoProvider`: a job's memory of its calls, so a resume repeats none (ADR-0020); `derived_seed()` |
| `src/library/include/rr/library/call_memo.hpp` | `CallMemo`: that memory, kept in the library database (`llm_memo`) |
| `src/library/include/rr/library/call_log.hpp` | `CallLog`: the records, kept in the library database |

## A call, as it is wired

Providers wrap providers, so each concern is written once and works with every backend:

```
structured<T>()                     schema, checks, one repair
  └ RetryingProvider                transient failures, with backoff; the sink hears on_restart()
      └ AccountedProvider           one record per attempt (the library's llm_calls)
          └ OpenAiProvider          one streamed attempt over HTTP
```

The hub builds this stack in one place, `hub::ModelStack` (`src/hub/model_stack.hpp`), and only against the runtime: ADR-0017's one model endpoint is a property of that code. It wires `llm.mode`, outermost first:

| Mode | Stack | Why the order |
|---|---|---|
| `live` | retries → accounting → the runtime | Retries above accounting: every attempt is a record, so a flaky pod shows as several attempts at one call |
| `record` | recording → retries → accounting → the runtime | The recording above the retries: a call is recorded once, after it succeeded |
| `replay` | retries → accounting → recorded answers | No runtime, no network. The accounting stays, so a replayed job reports what its recordings cost |

The benchmark builds the same stack without retries, since a failed call is a result for it, and refuses any mode but `live`. A replayed benchmark would measure nothing.

The vocabulary says *what* is asked, never how a server spells it:

- **`thinking` is a request, not a template flag.** llama-server spells it `chat_template_kwargs.enable_thinking`. To switch thinking off, the provider also sends `reasoning_effort: "none"`, which some templates honour instead.
- **`json_schema` is the model-facing schema's text** (`json::Schema::model_facing()`), with its fields in the order they were written; validation always uses the full schema. Why text: see "Field order is part of the prompt" below.
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

These facts come from llama.cpp's source (first read at `db00347`, then checked again at the image's pin, `7fe450e`), and the fake engine reproduces them:

- **The role chunk.** The first chunk's delta is `{"role": "assistant", "content": null}`, sent with the first token: the prompt is read in silence.
- **Every choice chunk carries its `index`.** With n choices (below), their chunks interleave.
- **Deltas.** Thinking arrives as `delta.reasoning_content` (the default `--reasoning-format` is `deepseek`), the answer as `delta.content`. Keys absent from a delta are missing, not null.
- **The finish chunk** has `"delta": {}` and `finish_reason`: `stop`, `length` or `tool_calls`.
- **Usage** is sent only when `stream_options.include_usage` asks for it: a chunk with `"choices": []`, whose `prompt_tokens` counts cached tokens too (`prompt_tokens_details.cached_tokens`). There is one per choice, written together with that choice's finish chunk (`format_oai_sse()` writes a result's chunks as one string).
- **`timings`** ride on the last JSON chunk: `prompt_per_second`, `predicted_per_second` and the rest. The engine's own speeds are what `bench` reports.
- **Errors.** Before the stream, an error is a plain JSON error with its status: 400 for a prompt longer than the context, 503 while loading. Mid-answer it is one error event, then the end, with no `[DONE]`.
- **Busy slots queue.** A queued streamed request gets no headers until a slot frees up. That silence is why the runtime answers `200` early and fills it with keep-alives.

### What the provider sends

```json
{"model": "commander", "messages": [...], "stream": true, "stream_options": {"include_usage": true},
 "max_tokens": 512, "temperature": 0.7, "top_p": 0.8, "top_k": 20, "min_p": 0.0,
 "presence_penalty": 0.0, "repeat_penalty": 1.0, "seed": 42,
 "response_format": {"type": "json_schema", "json_schema": {"name": "atom", "schema": {...}, "strict": true}},
 "chat_template_kwargs": {"enable_thinking": false}, "reasoning_effort": "none"}
```

With thinking on, the last line becomes `"chat_template_kwargs": {"enable_thinking": true}, "reasoning_effort": "medium", "reasoning_budget_tokens": 8192, "reasoning_budget_message": "…"` (see [Sampling](#sampling)).

- **The schema goes in OpenAI's shape**, the one llama-server reads it from. llama.cpp's README also shows `{"type": "json_schema", "schema": …}`, but the server's code ignores that `schema`.
- **The schema never reaches the prompt,** so prompts must describe the format themselves.
- **The schema keeps its field order on the wire.** The body is built as an ordered document (`json::OrderedJson`) and serialized as such; see below.
- **A message with one text part is sent as a plain string.** Every compatible server takes that form. Otherwise the content is an array of parts, with images as `image_url`.
- **The dialect decides the extensions.** `llama_server` reads `top_k`, `min_p`, `repeat_penalty`, the thinking fields and `timings`. `openai` refuses a request that sets any of them, because no standard field carries it (`presence_penalty` is standard, so both send it).
- **Every value is checked first** (`sampling_problems()`), NaN included; see [Sampling](#sampling).

## Field order is part of the prompt

A constrained decoder writes an object's fields in the schema's order. In llama.cpp at the image's pin (`7fe450e`):
- the server parses requests into `common_json`, whose objects "keep the order in which [keys] are added" (`common/json.h`);
- the schema reader keeps each object's properties in a list, in that order (`common/json-schema.cpp`);
- the grammar builder walks that list (`_build_object_rule`, `common/json-schema-to-grammar.cpp`).

So the schema's order is the order in which the model thinks *on the page*:
- `{"quote", "rationale", "decision"}` makes it quote its evidence, reason, then decide;
- `{"decision", "quote", "rationale"}` makes it decide first and justify afterwards. Worse, the justification then has a decision to defend.

`rr::json::Json` sorts object keys (that is what makes it canonical, ADR-0004). Before this was handled, every schema reached the model in *alphabetical* order, and nothing failed: the answers were valid, just reasoned in the wrong order. Now:

| Step | Keeps the order because |
|---|---|
| Loading a schema | `Schema::compile_text()` (and `SchemaRegistry::add_text()`) parse into `json::OrderedJson` |
| The model-facing form | `Schema::model_facing()` strips validation-only keywords from the ordered document, once, at compile time |
| The request | `ChatRequest::json_schema` holds that *text*; a `Json` value would sort it again |
| The request key | The text is part of it, so two orders are two different calls (their answers can differ) |
| The body | `chat_request_body()` returns an `OrderedJson`, and the provider serializes it as one |

`Schema::compile(const Json&)` still exists for schemas built as canonical values. Their properties reach the model alphabetically, so a schema meant for a model is loaded from text.

## Sampling

Sampling is part of the question: the same messages at another temperature, top-k or penalty are a different call, with a different request key. Three rules follow (`rr/llm/sampling.hpp`).

### 1. Every real call says how to sample

A field left unset takes the server's default. llama-server's defaults are not the model cards' (temperature 0.8, top-k 40, min-p 0.05), and they change between versions. So the hub's configuration holds **presets**, one per model and mode, and missions apply one (`apply()` fills what the request leaves unset; a mission can still choose its own temperature). Neutral penalties are sent too, so a server started with a penalty flag cannot change the question.

| Preset | Model, mode | temperature | top_p | top_k | min_p | thinking | budget | effort |
|---|---|---|---|---|---|---|---|---|
| `qwen_thinking` | Qwen3.8, the commander: planning, judging | 1.0 | 0.95 | 20 | 0 | on | 8,192 | medium |
| `qwen_direct` | Qwen3.8: drafting | 0.7 | 0.8 | 20 | 0 | off | — | — |
| `gemma_thinking` | Gemma 4, the infantry: escalations, judging | 1.0 | 0.95 | 64 | 0 | on | 4,096 | the model's |
| `gemma_direct` | Gemma 4: labelling, extraction | 1.0 | 0.95 | 64 | 0 | off | — | — |

The values are the model cards', as recorded when the plan was researched (2026-10-01). Two choices depart from them, on purpose:
- `qwen_direct` has no presence penalty, where the card recommends 1.5 (rule 3);
- `qwen_thinking` asks for medium effort, where the template's default, xhigh, over-thinks.

Runbook R1 confirms the presets against the real templates, and the eval's sweeps tune them (R6).

### 2. Values are checked before they are sent

`sampling_problems()` runs in the provider, before any byte leaves:
- **Ranges:** temperature ≥ 0 and finite; top_p in (0, 1]; top_k ≥ 0; min_p in [0, 1]; presence_penalty in [-2, 2]; repeat_penalty > 0; max_tokens ≥ 1.
- **NaN fails every range.** JSON has no NaN: nlohmann writes it as `null`, which a server reads as "use your default", a silent change of question. Each check is written so that NaN fails it (`v >= low && v <= high`, never `!(v < low || v > high)`).
- **Thinking's fields need thinking.** A thinking budget or a reasoning effort with thinking off (or left to the model) would be ignored without a word, so it is refused.

### 3. No penalties where the answer must stay close to its source

Repetition penalties (presence, frequency, repeat), DRY ("don't repeat yourself") and XTC ("exclude top choices") change the next token because of what was already written. In rReport's answers, repetition is the point:
- **A structured answer repeats its keys, quotes and labels.** The tenth atom is a "claim" as legitimately as the first, but a penalty pushes it toward a label not used yet: it biases the classification.
- **An article repeats names and terms, and quotes verbatim** (must-quote atoms, plan §3.1). A penalty pushes toward synonyms and paraphrase: it manufactures the softening and term drift that the Fidelity Ledger exists to catch. DRY penalizes repeated sequences, which is what a quote is; XTC removes the most probable tokens, often the exact word.

`structured()` refuses a request that sets any of them, in its fields or in `extra` (`penalty_problems()`), before any call. The writer (plan P20) applies the same rule. Qwen3.8's card suggests the penalty against endless repetition; rReport bounds that with `max_tokens` and validation instead: a looping answer fails its checks and is asked again.

### Thinking: budget and effort

What llama.cpp does with them at the image's pin (`7fe450e`, `tools/server/server-common.cpp`):
- **`reasoning_budget_tokens`** (alias `thinking_budget_tokens`). After that many tokens of thought, the sampler forces the template's end-of-thinking tag, preceded by `reasoning_budget_message`, and the model answers. It applies only when llama.cpp knows the template's thinking tags; runbook R1 checks each model. A thinking model therefore always answers, instead of spending its whole `max_tokens` thinking (Gemma 4 can loop). If a call still ends without an answer (`length`), it counts as invalid, and the retry runs with thinking off (ADR-0018).
- **`reasoning_effort`.** `"none"` turns thinking off. Any other value is handed to the chat template as its `reasoning_effort` argument. `ReasoningEffort` is an enum (low, medium, high, xhigh), so a misspelt level fails to compile, or to load from configuration, rather than being ignored by a template.

## Several answers from one prompt (`n`)

Test-time compute samples: k drafts of a section, k labellings of a window (ADR-0018). `ChatRequest::n` asks for them in one request, and llama-server makes that cheap. Its behaviour at `7fe450e`:
- **The prompt is read once.** The request's task gets n − 1 child tasks. When the parent has read the prompt, its attention cache is *copied* to the children's slots (`copy_state_to()`), so k samples cost one prompt.
- **Choice i is sampled with the seed + i** (`server_task::add_child()`). This is why derived seeds are spaced as ADR-0020 says.
- **It needs n free slots at once.** Until they are free, the request waits. With fewer slots in all it would wait forever, so a caller must never ask a model for more choices than its slots. `bench --n` checks this, and `Limiter::acquire(stop, priority, Slots{k})` holds k slots for k choices; it fails at once, rather than waiting forever, when k exceeds the limiter's capacity (`rr/core/limiter.hpp`).
- **Choice 0 is the request itself**, and the children are 1 … n − 1 (`server_response_reader::post_tasks()`).
- **Non-streamed, the usage is one answer's.** For n answers, llama-server's non-streamed response reports only one answer's completion tokens (seen at 7fe450e: two answers of 6 tokens, reported as 6). Streamed, each answer reports its own, and the reader sums them: one more reason every call streams. The live tests pin the streamed sum against a real llama-server ([runtime.md](runtime.md#the-real-engine-nightly-l4)).

How rReport reads such an answer (`ChatStreamReader`):

| | Rule | Why |
|---|---|---|
| Pieces | Each delta goes to its choice, by `index`, and the sink is told which (`on_content(choice, text)`) | The choices' tokens interleave |
| A choice's tokens | The usage chunk that follows a choice's finish chunk is that choice's | llama-server writes the two together |
| The call's tokens | Prompt: counted once (the largest any choice reports). Cached: likewise. Completions: summed | Each child reports the whole prompt, copied, not read again |
| A server that reports once | Taken at its word as the call's total, and credited to no one choice | OpenAI-style servers send one usage for the whole call |
| Speeds | Prompt speed: choice 0's. Generation: summed | The choices generate side by side |
| Endings | Every choice must finish, or the call fails (`unsupported`); `least_complete_finish()` says `length` if any was cut | A stream that loses a choice is not an answer |
| Indexes | One the request did not ask for, or one that is not a whole number ≥ 0, is refused | It is not llama-server's stream |

Two rules guard the request itself:
- **`n` > 1 needs sampling.** At temperature 0 every choice would be the same answer, so such a request is refused (`sampling_problems()`).
- **`structured_json()` takes one answer.** Each sample needs its own validation and repair conversation, which the army runs: it asks for a mission's samples in one request, then checks and repairs each on its own ([agents.md](agents.md#samples-share-a-prompt)).

## How sure the model was, token by token

A vote settles an item by the agreement of samples. A sample alone can still say how likely another would have agreed: the probability of each token it wrote (P28, `rr/reason/vote.hpp`). `ChatRequest::token_probabilities` asks for them, and each `Choice` comes back with `probabilities`: its content's tokens, in order, each a range of bytes and how likely it was when it was drawn.

What llama-server does at the image's pin (`7fe450e`: `tools/server/server-common.cpp`, `server-context.cpp`, `server-task.cpp`):
- **`logprobs: true` asks for them, `top_logprobs` how many alternatives**: 20 when it is absent, and `top_logprobs` without `logprobs` is refused with 400. With none, no token's probability is sent at all (`n_probs` 0). rReport asks for one, the least that sends the sampled token's own.
- **`post_sampling_probs: true` reports each token's probability after the sampler chain** (temperature, top-k, top-p, min-p), as `prob` and `top_probs`; without it, `logprob` and `top_logprobs`, of the model's raw distribution. After the chain is what a vote asks: the chance that the same chain, from the same prefix, draws the same token again. A token at 0.5 raw may be the only one top-p leaves, and drawn every time. It needs no logits kept from before sampling; the chain's candidates are sorted, few after top-k.
- **The grammar.** The chain's candidates are the unconstrained ones whenever the first draw fits the grammar, and the grammar's only after a draw it rejected (`common_sampler_sample()`, rejection sampling). A label the model writes unforced is the common case, and its probability is then the model's own.
- **Streamed, a token's entry rides on its chunk's last delta, and only when the token made one.** A token whose text the server holds back (part of a UTF-8 sequence, the start of a tag) sends no entry: the probabilities are best effort. A reasoning token's entry rides on its reasoning delta.

How rReport reads them (`ChatStreamReader`):

| Rule | Why |
|---|---|
| A chunk's entries are placed at the end of the content it adds, one after another, each as long as its `bytes` (else its `token`) | The entry is the chunk's last token: its text ends where the chunk's does |
| A token whose bytes reach back past that content is cut where the content began | Part of its text went elsewhere, a tag the server read |
| A chunk that adds no content is not read: its entries are the reasoning's | Thinking is not the answer, and how sure a model was while thinking says nothing of it |
| `prob` is taken as it is; a `logprob` through `exp()`; a probability a rounding above 1 is 1; anything else outside [0, 1], or an entry without its text, fails the call (`unsupported`) | A server that sends what is not a probability is not one rReport can read |
| A byte no entry covers is not known | And not known is not sure: a vote treats it as no confidence at all |

The flag is part of a request's key (an answer kept without probabilities has none to give), and only llama-server can carry it: elsewhere, `unsupported`. A response's JSON form, which the memo and the recordings keep, holds them as `[begin, end, probability]` triples, left out when there are none, so that no response kept before P28 reads differently; they are read strictly: in order, apart, within the content, in [0, 1].

The live tests (`EachTokensProbabilityComesWithItAndTheTokensCoverTheAnswer`) check what the placement assumes, on a real llama-server: an answer of ASCII words held to a schema, two of them drawn at once, each covered byte for byte, in order, by probabilities in (0, 1]. Run here against llama.cpp `7fe450e` built for the CPU and the tiny model, it passes.

## Answers that satisfy a schema

Missions ask for data, not prose, and nothing uses an answer before it is validated (plan §8). `structured<T>()` first refuses a request that sets a penalty ([Sampling](#sampling), rule 3), then:

1. **Sends the model-facing schema** where the provider supports constrained decoding, and switches thinking off unless the request says otherwise. A reasoning model under a grammar can spend its budget thinking, and some parsers constrain the thinking too (llama.cpp issue #20345).
2. **Parses and validates the answer.** A Markdown fence around the JSON is tolerated. The answer is checked against the *full* schema, then by the caller's own checks (what a schema cannot say), then converted to `T`. A conversion error is one more problem.
3. **Sends an unusable answer back once** (ADR-0007: one repair, then a fallback model). The model's own answer goes back, followed by a user turn listing every problem. An answer cut off at `max_tokens` is called out as such, since the only fix is a shorter answer.
4. **Fails with `model_output_invalid` if it is still unusable,** listing every problem, so the caller can escalate to another model.

Each attempt is an ordinary call: accounted for, retried and replayable like any other.

The steps are public too: `structured_request()` (1), `check_structured()` (2) and `repair_request()` (3). The army (`rr_agents`) runs the loop itself, so that each attempt takes an engine slot of its own, and an answer that cannot be repaired can climb to another model ([agents.md](agents.md#answers-checked-repaired-escalated)). Running the steps apart keeps one implementation of what an answer must be, and of how a repair is asked.

## Record and replay

Pipeline tests (layer L3) run whole videos without a GPU:

- **Keys.** A recording is `<directory>/<request key>.json`. The key is BLAKE3 over the canonical JSON of what the model sees, prefixed with the provider's name. Unset fields are left out of that JSON, so adding a field to `ChatRequest` later changes no existing key. The schema enters as its text, so its field order is part of the key.
- **Files.** A recording keeps the request, pretty-printed, so people can read it and review its diff. It is written atomically (`write_file_atomically()`).
- **Format 2.** A recording's response is a list of choices, so requests with `n` replay too, each choice to its index. A recording without `"format": 2` (format 1 held one answer) is refused, never guessed at: record the call again. No format-1 recording was ever committed.
- **What is recorded.** Only successes. A replayed transient failure would make a test depend on luck.
- **Replay is strict.** A request without a recording fails with `not_found` and names its key. A recording copied under another call's name is refused as `integrity`.

## A job's memory of its calls

A job makes thousands of calls over an hour or more, and a pod stop, a crash or Ctrl-C can interrupt it anywhere. A finished stage is a cache hit (ADR-0004); an interrupted one would start over. `MemoProvider` wraps a job's stack (above everything `ModelStack` builds) and remembers each call's answer as it arrives, so a resumed job repeats at most the calls that were in flight (ADR-0020).

| Rule | Why |
|---|---|
| The key is the request's key **and the served model's checksum** (`memo_key()`) | A request names a model id, and the file behind it can change between a crash and a resume: a catalog re-pinned to a re-uploaded file keeps its id. The runtime reports each engine's checksum in its health report (`model_sha256`, protocol v2) |
| A request that may sample needs a seed: temperature unset (servers then sample) or above 0, without one, is refused | Otherwise three samples of a window are three identical requests, and all three get the first one's answer |
| Seeds are derived: `base + sample`, with `base = BLAKE3(salt, stage, mission) mod 2³¹` (`derived_seed()`) | Deterministic, so a resume asks the same questions. Consecutive, so a request for n choices (llama-server: choice i gets seed + i) draws the same seeds as n single requests. Below 2³² − 1, which llama-server reads as "pick one at random" |
| Only successes are kept, the first under a key stays | A failure is retried, never replayed |
| A failure to keep an answer is logged; the answer is still returned | Losing a memo entry costs one repeated call, never the answer already paid for |
| A memo that cannot be read fails the call | An unreadable store is a fault to report, not a reason to pay for the call again unnoticed |
| A remembered answer reaches the sink whole, choice by choice, and is not accounted again | It was accounted when it was made |

`CallMemo` keeps the answers in the library database (`migrations/0002_call_memo.sql`: `llm_memo`, keyed by job and memo key, one row per answer, committed as it arrives), next to the job's artifacts and as durable as they are. `forget()` deletes a job's rows once its results are committed. That a resumed job asks again only what its memo did not keep is proven through the real executables, a job killed with SIGKILL and resumed ([testing.md](testing.md#end-to-end-killed-and-resumed)). The derivation of seeds is pinned by a test against values computed independently (Python's `blake3` over the same canonical JSON): changing it would orphan every memo, and break the eval's common random numbers between versions.

## Accounting

Tokens and GPU time are the product's cost (plan §4.3). `AccountedProvider` hands one `CallRecord` per attempt to a `CallRecorder`. `rr_library`'s `CallLog` keeps records in the library database (`migrations/0001_accounting.sql`):

- **`llm_calls`** holds one row per attempt: start, duration, time to the first token, provider, model, tag, request key, status (`ok` or the error class), finish reason and tokens. With n choices the finish reason is the least complete one (`length` if any answer was cut), and the tokens are the call's, as above.
  - Attempts of one call share a request key, so a flaky pod shows up as several rows, not one slow call.
  - A CHECK ties `status` and `error` together.
- **`llm_call_payloads`** holds what was sent and received, apart from the numbers, so `prune_payloads()` can apply a retention policy without touching the accounting.
- **Recording never fails a call.** An answer the model already produced, and the GPU already paid for, is not thrown away because a row could not be written. The failure is logged.

`rr_llm` knows nothing of SQLite: the library implements the recorder interface. This keeps `rr_llm`'s dependencies as the plan's §7 table has them.

## How it is tested

| Level | What |
|---|---|
| Unit, scripted provider | `tests/unit/llm/scripted_provider.hpp` plays answers and errors, and keeps every request it was given. It covers retries, restarts, accounting, record/replay and `structured<T>()`, repair conversation included |
| Unit, scripted server | `openai_provider_test.cpp` serves exact bytes: llama-server's stream shape, keep-alive comments, a missing `[DONE]`, error events and statuses, silence and cancellation; token probabilities asked after the sampler chain, placed on the bytes they wrote, refused when they are not probabilities, and the fake engine's own stream of them read back |
| End to end | `runtime_client_test.cpp` calls through the real runtime's relay to fake engines. Tokens, reasoning, usage and timings arrive intact; a 400 stays a bad request; a crash mid-answer becomes a retried network error; an engine's error event keeps its class |
| The library | `call_log_test.cpp` round-trips every field, NULLs included; covers filters, newest first, pruning, and a duplicate refused without a trace |

### How the tests were validated

Each rule was removed on purpose, and the named test was confirmed to fail. (Mutants are applied by pattern, a pattern must match exactly once, and the source is restored after each run.)

Two findings from these runs were fixed before they counted:
- **A crash is not a failure message.** Three provider mutants were first caught only because a test read a missing key through a *const* `json::Json`, whose `operator[]` asserts and aborts the whole test program. The test now reads a non-const copy, which yields null, so the expectation fails with a message.
- **A refactor must not hide a mutant.** `chat_request_body()` grew past clang-tidy's complexity limit and was split into helpers. Every mutant against it was then run again, on the new shape.
- **Tests too kind to tell rules apart.** Before the `n` mutants ran, three tests were sharpened so that a wrong rule could not pass them by coincidence:
  - the "reports once" stream now carries a true total, and no choice may be credited with it;
  - the two choices report different prompt speeds, so "first seen" differs from "choice 0's";
  - an index that is not a whole number is tried, as a string, a negative and a fraction.

  The bounds check on an index has no mutant: without it, an unasked-for index would be written past the end of the choices, which only a sanitizer reports reliably. It is left to the ASan build, which runs `AStreamMustFinishEveryChoiceItWasAskedFor`.

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
| The schema's field order is part of the key | `TheSchemasFieldOrderIsPartOfTheKey` |
| The body stays ordered until it is serialized | `TheSchemaReachesTheServerInItsOwnOrder` |
| The schema is parsed in order | `TheSchemaReachesTheServerInItsOwnOrder` |
| A schema that is not JSON is refused | `RefusesWhatItCannotCarry` |
| `structured()` sends the ordered model-facing form | `AsksForTheFieldsInTheOrderTheSchemaWasWritten` |
| `rr_json`: `parse_ordered()` and its `dump()` keep the order | `OrderedJson.KeepsMembersInTheOrderWritten` |
| `rr_json`: text and ordered documents compile in order | `Schema.TheModelGetsTheFieldsInTheOrderWritten` |
| `rr_json`: the registry loads text in order | `SchemaRegistry.SchemasLoadedFromTextKeepTheirOrder` |
| `rr_json`: stripping keeps an ordered document's order | `ToModelFacing.KeepsTheOrderOfAnOrderedDocument` |
| `rr_json`: validation keeps the full rules | `Schema.TheModelGetsTheFieldsInTheOrderWritten` |
| A preset fills only what the request leaves unset, top_k included | `APresetFillsWhatTheRequestLeavesOpen` |
| A budget and an effort come from a preset only with thinking | `ABudgetAndAnEffortComeOnlyWithThinking` |
| NaN fails every range; infinity fails the temperature | `ValuesAreCheckedAndNanFailsEveryRange` |
| A budget or an effort needs thinking; a budget is ≥ 0 | `ABudgetAndAnEffortNeedThinking` |
| A preset that sets thinking's fields must think | `APresetIsCheckedAsAWhole` |
| Penalties are found in `extra` too, a non-number included | `PenaltiesAreFoundInTheFieldsAndInExtra` |
| Values are checked before sending | `RefusesWhatItCannotCarry` |
| llama-server's fields are refused on a plain OpenAI server | `RefusesWhatItCannotCarry` |
| A schema needs the capability | `RefusesWhatItCannotCarry` |
| `top_k`, the effort, the budget and its closing are sent | `SendsSamplingAndThinkingAsLlamaServerReadsThem` |
| An empty closing is not sent | `SendsSamplingAndThinkingAsLlamaServerReadsThem` |
| Thinking off says `reasoning_effort: "none"` | `SendsWhatLlamaServerReads` |
| The budget's closing is part of the key | `TheRequestKeyIsWhatTheModelSees` |
| `structured()` refuses penalties, before any call | `RefusesPenaltiesBeforeAnyCall` |
| Each piece goes to its choice's index, and the sink is told which | `ReadsEveryChoiceByItsIndex` |
| A usage chunk is credited to the choice that just finished | `ReadsEveryChoiceByItsIndex` |
| The prompt counts once; completions and speeds add up; prompt speed is choice 0's | `ReadsEveryChoiceByItsIndex` |
| `n` is sent, and is part of the key | `SendsSamplingAndThinkingAsLlamaServerReadsThem`, `TheRequestKeyIsWhatTheModelSees` |
| The least complete finish is reported | `ReadsEveryChoiceByItsIndex` |
| Every choice must finish | `AStreamMustFinishEveryChoiceItWasAskedFor` |
| Token probabilities are asked: `logprobs`, one alternative, after the sampler chain; of llama-server alone | `AsksHowSureTheModelWasAfterItsSamplerChain` |
| A token is placed at the end of its chunk's content, and cut where that content began; a `logprob` is exponentiated | `PlacesEachTokensProbabilityOnTheBytesItWrote` |
| A probability outside [0, 1] is refused, either way; a reasoning token's entry is not read | `RefusesATokenProbabilityThatIsNotOne` |
| The flag is part of the key | `TheRequestKeyIsWhatTheModelSees` |
| A response keeps its probabilities; overlapping ones, ones past the content, or outside [0, 1] are refused | `AResponsesTokenProbabilitiesSurviveItsJsonForm` |
| A total reported once is credited to no choice | `AServerThatReportsUsageOnceIsTakenAtItsWord` |
| `n` ≥ 1, and `n` > 1 needs sampling | `ValuesAreCheckedAndNanFailsEveryRange`, `SeveralAnswersNeedSampling` |
| `structured()` takes one answer | `TakesOneAnswer` |
| Recordings carry their format; an older one is refused | `RecordsAnswersAndReplaysThemWithoutAModel`, `ARecordingInAnOlderFormatIsRefusedNotGuessedAt` |
| Replay streams each choice to its index | `EveryChoiceIsRecordedAndReplayedToItsIndex` |
| The record's finish is the least complete choice's | `ACallOfManyChoicesIsRecordedAsTheLeastCompleteOfThem` |
| Memo: a sampled request needs a seed; an unset temperature samples | `ARequestThatMaySampleNeedsASeed` |
| Memo: the served model is part of the key | `TheServedModelIsPartOfTheKey` |
| Memo: answers are kept, and given back from memory | `AnAnswerGivenOnceIsGivenFromMemoryAfterwards` |
| Memo: a failure to keep is not fatal; a failure to read is | `AnAnswerOutlivesAFailureToKeepIt`, `AMemoThatCannotBeReadOrAModelThatCannotBeNamedFailsTheCall` |
| Memo: seeds are spaced by sample, salted, masked below 2³¹, and pinned | `SeedsAreDerivedSoThatAResumeAsksTheSameQuestions` |
| `CallMemo`: one memory per job; the first answer stays; forgetting one job leaves the others | `EachJobHasItsOwnMemory`, `TheFirstAnswerStays` |
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

**P28's run** killed 14 of its 15 at once. The fifteenth, a token not cut where its chunk began, did not build: it left the chunk's beginning unused, which `-Werror` refuses. Written to keep it (`begin * 0`), it is killed, by the test that claims it. And clang-tidy, asked to simplify the probability checks by De Morgan's laws, would have let NaN through: `p < 0 || p > 1` holds for no NaN. Each check names NaN now (`std::isnan(p) || p < 0 || p > 1`), as [the rule on ranges](#sampling) wants.

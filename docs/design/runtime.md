# The pod runtime: engines, the relay, blobs and idle stop

`rreport-runtime` runs on the rented GPU pod. It is the pod's one HTTP door, and everything behind that door is either its own code or a child process it manages. The code is small, but each rule below exists because of how GPUs, llama-server and RunPod's proxy actually behave. They are recorded here so nobody "simplifies" one away.

## Pieces

| Where | What it is |
|---|---|
| `src/runtime_api/` (`rr_runtime_api`) | The hub ↔ runtime protocol v2: endpoint paths, the health report and its strict JSON form, and library-name validation. Linked by both sides |
| `src/supervisor/catalog.*` | The model catalog (TOML): models with file, sha256 and URL, and GPU profiles that say which engines run with which slots and context, and whether the slots share one pool |
| `src/supervisor/gpu.*` | `nvidia-smi` CSV parsing: name, memory, utilization, driver |
| `src/supervisor/engine.*` | One engine = one `llama-server` child process, kept alive (lifecycle below) |
| `src/supervisor/models.*` | Model files: verified against the catalog's sha256 (with a safe cache), downloaded and resumed when missing |
| `src/supervisor/proxy.*` | The relay: an OpenAI-compatible call in, the engine's answer streamed out (ADR-0014) |
| `src/supervisor/watchdog.*` | When the pod should stop itself |
| `src/supervisor/supervisor.*` | The wiring: routes, the auth guard, blobs, engines, and the watcher thread |
| `src/supervisor/runtime_config.*` | From configuration to `SupervisorSettings`: aliases, the `auto` profile, RunPod defaults |
| `apps/rreport-runtime/` | The executable: commands, signals, exit codes |
| `tests/fake_engine/` | `rr_fake_engine`: a scriptable llama-server stand-in, so every behaviour is tested end to end without a GPU |
| `docker/runtime/` | The pod's image: the Dockerfile, the entry point (PID 1) and the model catalog (see "The image") |

## The door

Every endpoint except `GET /ping` requires `Authorization: Bearer <token>`.
- **Case-insensitive scheme.** The scheme is matched case-insensitively (RFC 9110).
- **Constant-time comparison.** The token is compared in constant time (`Secret::matches`).
- **Checked before any body is read.** The guard runs before routing (`rr_net`), so an unauthenticated client cannot make the pod buffer a multi-gigabyte upload.
- **The answer to a failure.** A 401 carries `WWW-Authenticate: Bearer`.

## Engines

Each resident model is served by its own `llama-server` process on `127.0.0.1`, at consecutive ports from `first_engine_port` in profile order. ADR-0003 explains why these are processes: a crash cannot take the supervisor down, and VRAM is certainly released when a process exits.

```
starting ──(GET /health answers 200)──▶ ready
   │  ▲                                  │
   │  └──── restart, with backoff ◀──────┤ the process exited
   ▼                                     ▼
 failed (not ready within startup_timeout, or 4 consecutive failures)
```

- **Readiness is llama-server's own signal.** `/health` answers 503 while the model loads, which takes minutes for a large model, then 200.
- **Restarts back off.** The pause starts at 2 s and doubles per consecutive failure, up to 60 s.
- **The failure count resets once the engine is ready.** A model that works but crashes once a day is not given up on.
- **Every wait is interruptible.** Stopping sends SIGTERM to the engine's process group, then SIGKILL after the grace period (ADR-0013).
- **Model calls need a ready engine.** A call for a model whose engine is not ready gets `503 unavailable`. While the engine is still starting, the answer also carries `Retry-After: 10`.
- **Each engine reports its context, as the engine itself has it.** The hub sizes its work by these figures (see the next section), so they are never guessed:
  - `context`: tokens of attention cache, all slots together (`--ctx-size`);
  - `ctx_per_slot`: the most one sequence (a prompt and its answer) may hold;
  - `kv_unified`: whether the slots share one pool;
  - `model_sha256`: the checksum of the file served, as verified before loading. It is what a model id means right now, and a job's memo is keyed by it (ADR-0020).

  `ctx_per_slot` starts as the catalog's arithmetic. Once `/health` answers 200, and **before** the engine is declared ready, the supervisor reads llama-server's own figure from `/props` (`default_generation_settings.n_ctx`). That figure wins, because llama-server caps the arithmetic, for one at the model's training context. When the engine cannot say (no `/props`, or a figure of 0 or less), the catalog's figure stands, and the discrepancy or the failure is logged.

## Context per slot, and the shared pool

llama-server divides an engine's `--ctx-size` evenly between its `--parallel` slots, unless the slots share one pool (`--kv-unified`). Then one sequence may grow to the whole pool. The catalog says which, per engine (`kv_unified = true`), and the supervisor states it on the command line either way (`--kv-unified` or `--no-kv-unified`), so a `LLAMA_ARG_KV_UNIFIED` in the pod's environment cannot change what the catalog says.

**Why the commander's slots share a pool** (the 48 and 80 GB profiles):
- **A request for n answers keeps its prompt once.** Its choices share the prompt's cache cells. With an even split, each choice's slot holds its own copy, and the prompt must fit one share.
- **A long prompt may use the room that idle slots leave.** The commander reads whole transcripts to outline them. With four slots and an even split of 32,768 tokens, nothing over 8,192 would fit.
- **It costs no attention-cache memory.** The pool keeps the size the one slot had.

**Its price: requests at once must fit the pool together.** When they do not, llama-server fails **every** request in flight with "Context size has been exceeded", and keeps running. It does not restart: the error is caught inside its loop (`update_slots`, llama.cpp 7fe450e). With an even split, nothing one request does touches another: a prompt larger than its share is refused before it starts (`400 exceed_context_size_error`), and an answer that would outgrow the share stops there (`finish_reason: length`, since context shift is off). So whoever sends requests to a shared pool must budget them in tokens, not only in slots. The pipeline's army will (P8).

**What the catalog refuses**, so that mistakes surface when it loads, not when an engine fails to start or quietly runs with less:
- unknown keys at any level (a misspelt `kv_unified` would otherwise leave each slot a quarter of the context);
- values of the wrong type (a quoted `slots = "4"` would otherwise become the default, 1);
- in `extra_args`, the flags the supervisor sets from the catalog's own fields (`--model`, `--alias`, `--host`, `--port`, `--parallel`, `--ctx-size`, `--kv-unified` and its kin, `--mmproj`, `--embedding`, `--rerank`, in their short forms too). llama-server keeps the last of two values, so a second `--parallel` would silently overrule `slots`, and the health report would be wrong;
- in `extra_args`, flags that make the engine load or fetch a file the catalog has not verified (`-hf`, `--hf-file`, `--model-url`, `--docker-repo`, `--mmproj-url`, `--lora`, a draft model), and `--api-key`, which would make the engine refuse the supervisor's relay. llama.cpp reads `_` and `-` alike in a long flag (`--ctx_size`), and so does the check.

## The relay (ADR-0014)

The hub always streams long calls. The relay's job is to make the stream survive the path and to make every ending honest.

**Threads.** The engine's transfer runs on a thread of its own. It hands chunks to the downstream side through a buffer bounded at 4 MiB, so a slow hub slows the engine's transfer down (TCP back-pressure) instead of making the pod hold a whole generation in memory.

**What each situation looks like to the hub:**

| Situation | What the hub receives |
|---|---|
| The engine answers 2xx | Its body, streamed as it arrives |
| The engine refuses in time (e.g. prompt over the context size) | Its status and body, whole |
| A streamed call, and the engine has not answered within `stream_head_patience` (1 s) | `200 text/event-stream` from the relay itself, then the engine's body when it comes |
| …and the engine then refuses | One event `data: {"error": {...}}` carrying the exact `Errc`; the stream ends cleanly |
| Silence in a streamed call | `: keep-alive` after each `stream_keep_alive` (15 s), only at a line start |
| The engine breaks off mid-answer | An error event where one fits (between events), then the transfer is cut short: `Errc::network`, retried |
| The runtime stops | `unavailable` within 100 ms, as a status or an event |
| The hub leaves | The engine's transfer is cancelled at once; llama-server frees the slot |

**Why each rule:**

- **Keep-alives exist because of RunPod's proxy.** It closes connections after about 100 s without a byte. A request queued behind busy slots, or reading a long prompt, sends nothing: llama-server's own pings start only after its `200`, and older builds have none.
- **Keep-alives go only at a line start.** A comment line is ignored wherever a line may start, even inside a multi-line event, while a blank line would end an event early. A CR does not count as a line start, because it may be half of a CRLF. `KeepAlivesNeverSplitAnEvent` pauses every event in the middle of its line, for three times the keep-alive interval.
- **Error events go only between events.** Otherwise a pending `data:` line would merge with the inserted event into garbage. `ABreakInsideAnEventLeavesNoGarbage` breaks off after one whole line of an event.
- **A broken answer is cut short.** An error event alone would be enough for our hub, but any client that ignores error events would take the truncated answer for a complete one. With nothing of an answer relayed yet, the error event is the whole body and the stream ends cleanly.
- **Every wait is one tick (≤100 ms).** Waiting blindly for an engine's head, or for its next byte, would hold `Supervisor::stop()` for as long as an engine stays silent: up to `proxy_idle_timeout`, 10 minutes. Instead the relay re-checks, once a tick:
  - the shutdown token;
  - whether the client is still there (`Request::client_gone` in the handler; an empty write, which only probes, in the stream);
  - the keep-alive timer.
- **A stop never cuts an answered stream short.** The server lets every stream already answered begin before it closes its socket, since httplib begins no body after that. So a relay answered a moment before the stop still sends its `unavailable` event and ends cleanly (`docs/design/networking.md`; CI caught `StoppingEndsSilentStreamsPromptly` failing in that gap).
- **Only `Content-Type` and `Accept` are forwarded.**
  - The hub's bearer token is the pod's secret.
  - llama-server acts on some headers: with `X-Conversation-Id` it keeps generating after the client disconnects, for nobody.
- **`"stream"` must be a boolean.** Anything else is the client's mistake: a 400, not a 500 from a JSON type error.
- **Exchange lifetime.**
  - The handler and the stream function share one `Exchange` (bridge, in-flight guard, options, upstream thread).
  - The upstream thread holds a plain pointer to it, never a `shared_ptr`: the last owner destroys the thread's `jthread`, and a thread cannot join itself.
  - Cleanup lives in destructors, because httplib may never call a stream function (the client left before the headers were flushed).

## Blobs

- **Endpoints.**
  - `PUT /v1/blobs/{library}/{blake3}` streams an upload into the library's content-addressed store (`rr_blob`).
  - `commit()` verifies the content against the name's hash. A mismatch stores nothing and answers `422 integrity`.
  - `HEAD` answers 200 with `X-Blob-Size`, or 404. It also refreshes the blob's timestamp, since a HEAD means "about to use it". `GET` answers 405, because blobs flow one way.
- **Library names become directory names.** They are validated (1–64 of `[a-z0-9_-]`, not starting with `-`), so a name can never escape the blob root.
- **Unused blobs are deleted.** Blobs not used for `blob_retention` (24 h) are removed by the watcher hourly.

## Idle stop: the dead-man switch

A pod bills every minute it runs, so it must stop on its own when nobody needs it, including when the hub that wanted it is gone (the laptop slept, the hub crashed).

- **Activity.** Activity is a model call, a blob request or the hub's heartbeat.
- **What doesn't count.** `/ping` and `/v1/health` are not activity: monitoring must not keep a pod alive.
- **In-flight requests.** A request in flight is never idle, however long it runs. The relay's `Exchange` holds the `InFlight` guard until the stream ends.
- **The idle period.** It starts when the last request *ends*.
- **The stop.** After `idle_stop` of idleness the watcher stops the pod, and tries again every 5 minutes while the pod is still up (ADR-0015).
  - On RunPod the runtime calls RunPod's API itself (`rr_runpod`, `stop_self()`): REST v2 first, then GraphQL's `podStop` if REST refuses the key. Reports say the pod-scoped key RunPod provides (`RUNPOD_API_KEY`) is refused by REST and accepted by GraphQL.
  - The key is `idle.runpod_api_key` if set, else `RUNPOD_API_KEY`. A key of the owner's own, passed as a RunPod secret, works either way.
  - `runpodctl` is not used: custom images may lack it, and its two generations disagree on the command's form and the API behind it.
  - A configured `idle.stop_command` wins (for machines other than RunPod).
- **Nothing to stop with** (no command, no key). The runtime only logs, loudly.
- **What a stop keeps.** A stopped pod keeps its `/workspace` volume (models, blobs) and loses its container disk, so nothing the runtime needs lives outside `/workspace`. It resumes on the same machine only: when that machine's GPUs are taken, RunPod refuses the start, and the hub says so (`rr_runpod` classifies it as `unavailable`).

## Model files: verified before use, fetched when missing

Engines load multi-gigabyte GGUF files from the pod's network volume. A missing, truncated or corrupted file makes llama-server fail minutes into loading, or load garbage. So before the engines start, every file the chosen profile needs is checked against the catalog's sha256 (`src/supervisor/models.*`).

- **Verification is cached, safely.** Hashing tens of gigabytes on a network volume takes minutes, and pods restart often.
  - After hashing, a sidecar `<file>.verified` records size, mtime, **ctime** and inode with the hash.
  - A later check trusts the record only while all four still match. ctime changes on every write and cannot be set from user space, unlike mtime, which `cp -p` and `rsync -t` preserve.
  - The file is stat'ed before and after hashing. A file that changed meanwhile is `Errc::conflict`, and nothing is recorded.
  - `models verify --full` ignores the records.
- **Downloads resume.**
  - The bytes collect in `<file>.<first 16 hex of the sha256>.partial`. The expected hash is in the name, so a partial of an older version is never continued into a newer one.
  - A resume first re-hashes the bytes it has, then asks for `Range: bytes=N-`:
    - `206` must start exactly at N (checked against `Content-Range`); anything else is never spliced in.
    - `200` means the range was ignored, so the download starts over.
    - `416` means nothing is left, so what is there is verified.
  - Compression is declined, so offsets always refer to the stored bytes.
- **Only verified content is published.**
  - The data is hashed while it is written, so a finished download needs no second read.
  - Then fsync, rename, directory fsync, and the record.
  - A mismatch deletes the partial and downloads again from the start, once: the source may have moved between attempts. A second mismatch is `integrity`, and nothing is stored.
- **Tokens stay with their host.** `HF_TOKEN` goes only to the Hub's own hosts (`huggingface.co`, `hf.co`, exact match). The Hub redirects downloads to a CDN with a signed URL, and libcurl drops `Authorization` on redirects to another host. A catalog URL not pinned to a commit (`/resolve/<40 hex>/`) is warned about, since its content can change under the sha256.
- **Retrying is patient with progress.** Only transient failures are retried, with full-jitter backoff. Any transfer that brings new bytes resets the budget of attempts without progress (5 by default), so a 20 GB download that stalls now and then completes, while one that fails repeatedly without a byte gives up.
- **Free space is checked first**, against the size the server announces, before any byte is written.

## The executable: `rreport-runtime`

`apps/rreport-runtime/main.cpp` only parses arguments and wires modules together. The rules live in `src/supervisor/runtime_config.*` and are unit-tested.

| Command | What it does | Exit codes |
|---|---|---|
| `serve` | Verifies the profile's model files, starts the engines and the server, prints `listening on <host>:<port>`, and stops cleanly on SIGTERM, SIGINT or SIGHUP | 0 after a clean stop; 1 when it cannot start (the port is taken, say); 3 when model files are missing or wrong |
| `models verify [--full]` | Checks the profile's model files (`--full`: hash everything, ignoring the records) | 0 all verified; 3 otherwise |
| `models sync` | Downloads what is missing or wrong, verifying everything | 0; 1 on failure |
| `gpu` | Lists the GPUs `nvidia-smi` sees | 0; 1 without a GPU |
| `stop-pod` | Stops this pod through RunPod's API, as an idle pod does. The image's entry point runs it when the pod cannot start, so it needs only RunPod's variables and the `idle` settings: no GPU, no token, no catalog (ADR-0016) | 0; 1 when RunPod refuses; 2 when not on RunPod or without a key |
| `config show [--origin]` | Every setting, secrets masked, optionally with where it came from | 0 |
| `version` | Version and git revision | 0 |

**Common exit codes.** For every command, 1 also means an unexpected error, and 2 a usage or configuration error. 128+N means signal N interrupted work in progress: a verification or a download. Once `serve` is serving, a signal is the normal way to stop it, and it exits 0.

- **Configuration.**
  - The built-in defaults, `config/runtime.default.toml`, are compiled in (`rr_add_embedded_library`) and are also the schema. A misspelt key or a wrong type fails at startup, with a suggestion.
  - Layers, each overriding the ones before: defaults < `--config` (or `RR_RUNTIME_CONFIG`) < `--secrets` < `RR_RUNTIME_<SECTION>__<KEY>` < `--set section.key=value`.
- **Environment names.**
  - The prefix is `RR_RUNTIME_`, not `RREPORT_`. The hub owns `RREPORT_` and refuses keys it does not know, so the two programs must not share a prefix on one machine.
  - Two aliases are accepted: `RR_RUNTIME_TOKEN` (`server.token`) and `HF_TOKEN` (`models.hub_token`). An alias that disagrees with its canonical variable is an error, never a silent choice.
- **Profiles and the stop command.**
  - `models.profile = "auto"` runs the profile the smallest GPU holds; a profile lays models out on one GPU.
  - On RunPod (`RUNPOD_POD_ID` set) with no stop command configured, an idle pod stops itself through RunPod's API (see "Idle stop" above).
  - `"0s"` turns idle stop and keep-alives off.
- **Signals (`rr::proc::TerminationSignals`).**
  - SIGINT, SIGTERM and SIGHUP are blocked in the first line of `main()`, before any thread exists, so every thread inherits the mask. One thread takes them with `sigtimedwait()`, which turns them into an ordinary stop request. No signal handler runs, so nothing has to be async-signal-safe.
  - This works as PID 1 in a container, where a signal without a handler would be ignored: the kernel never ignores a blocked signal.
  - Children get a clean mask (ADR-0013). SIGPIPE is ignored: a write to a closed pipe is an error to handle, not a reason to die.
- **Unexpected exceptions.**
  - Every failure the program expects travels as a `Result`. An exception that reaches `main()` is a bug, or the system running out of memory or threads.
  - `main()` catches it, logs it (to the log file too, once logging is up; errors are flushed at once) and exits 1.
  - Uncaught, it would call `std::terminate()`, which need not unwind the stack first. The supervisor's destructor, which takes the engines down, might then never run.
- **Component tests.** `tests/component/runtime_app_test.cpp` runs the real binary against `rr_fake_engine`:
  - it answers, and needs the token;
  - SIGTERM gives exit 0 with the engine's port closed;
  - no token gives exit 2 naming `RR_RUNTIME_TOKEN`;
  - a tampered model file gives exit 3 pointing at `models sync`;
  - a misspelt setting, given after the command, gives exit 2 with the configuration loader's suggestion;
  - `config show` never prints the token.
- **Two traps met on the way.**
  - **CLI11's `fallthrough`** (global options after a command) is inherited only by commands created *after* it is set.
  - **`rr_add_embedded_library`** wrote its header through a CMake bracket argument `[[…]]`, and the header's own `[[nodiscard]]` ended it early. It is now `[=[…]=]`. This was its first use.

## The image

`docker/runtime/Dockerfile` builds the pod's image in three stages. `.github/workflows/runtime-image.yml` pushes it to `ghcr.io/<owner>/rreport-runtime` on `main`, on tags, and by hand for a branch.

1. **llama.cpp** at a release tag, checked against the commit it must name (`v0.5.0` = `7fe450e`), since a tag can be moved.
   - It is built on NVIDIA's CUDA 12.8.1 devel image, as llama.cpp's own CUDA image is built.
   - Architectures `80;86;89;90;120` cover the A100, the 30- and 40-series, the L40S, the H100 and the 5090 (sm_120 needs CUDA 12.8).
   - Backends are libraries loaded at run time, with a CPU backend for every x86 generation, because the pod's CPU is unknown at build time.
2. **rreport-runtime**, with the `runtime-release` preset (no hub, no tests), from the same package list as CI (`scripts/install-deps-ubuntu.sh`).
   - The build names its revision like the hub (`git rev-parse --short=12`), or takes it as an argument (`RR_GIT_REVISION`), so `rreport doctor` can compare the two exactly.
3. **The image**, on the CUDA runtime base, holding only the binaries, their libraries, the catalog and the entry point. Model files live on the pod's `/workspace` volume, never in the image.

**The entry point** (`entrypoint.sh`, PID 1):
1. Checks for a GPU (`nvidia-smi`) and the hub's token.
2. Runs `rreport-runtime models sync`.
3. Runs `serve` until the pod is stopped.

Any failure stops the pod through RunPod's API instead of exiting, because RunPod restarts a container whose command exits, and a pod that cannot start would restart and bill forever (ADR-0016). Bash stays PID 1 to see how the runtime ended, and it passes stop signals on, since the kernel gives PID 1 no default signal handling. `test_entrypoint.py` runs the real script with fake `nvidia-smi` and `rreport-runtime` executables and checks every path, signals included. Each of its ten mechanisms was removed on purpose and caught.

**What was checked before any pod ran** (2026-09-30):
- **Built with Docker in the development sandbox:** the rReport stage and the final stage, command for command, with plain Ubuntu 24.04 (what the CUDA images are built on) in place of the CUDA base. The packages resolve, and the binary runs in the image and names its revision as the hub does.
- **Run as PID 1**, without a GPU, against a stand-in for RunPod's API:
  - the entry point logged its reason and stopped the pod (REST refused the pod's key, GraphQL took it);
  - it then kept waiting;
  - `docker stop` ended it in 0.1 s, with exit code 0.
- **Not built locally:** llama.cpp's CUDA stage (an hour of compilation). The image workflow builds it first, and the runbook's checks exercise it on a real GPU.

**The catalog** (`docker/runtime/catalog.toml`) holds one profile per GPU size, following the plan's §4.5. `auto` picks the largest profile the GPU holds (a unit test checks the choice for the memory real GPUs report):

| Profile | GPUs | Engines |
|---|---|---|
| `16gb` | RTX A4000, L4, A10 | Gemma 4 E4B (8 slots), the embedder: a cheap pod to smoke-test the image |
| `24gb` | RTX 4090, 3090, A5000 | Qwen3.8-27B Q4 alone: commander and infantry phases alternate at this size |
| `24gb-infantry` | the same, chosen by name | Gemma 4 26B-A4B (8 slots) |
| `32gb` | RTX 5090 | Qwen3.8-27B Q4 and Gemma 4 12B (6 slots), tight |
| `48gb` | L40S, A40, A6000 | Qwen3.8-27B Q4 (4 slots, one shared pool), Gemma 4 26B-A4B (16 slots), the embedder |
| `80gb` | A100, H100 | Qwen3.8-27B Q8 (4 slots, one shared pool), Gemma 4 26B-A4B (32 slots), the embedder |

- **Every engine runs with the same flags:**
  - every layer on the GPU (`--n-gpu-layers 999`), so a model that does not fit fails loudly instead of running partly on the CPU;
  - an 8-bit attention cache (`--cache-type-k/v q8_0`, which needs `--flash-attn on`);
  - the model's own chat template (`--jinja`).
- **Contexts and slot counts are estimates** until `rreport bench` measures them: [runbook R1](../runbooks/m1-m2.md#r1-the-commanders-slots-and-the-prompt-cache) settles the commander's.
- **Two differences from the plan's layout** (§13, M0 step 4):
  - Its `models.toml` and `profiles/*.toml` are one file, `catalog.toml`, because the catalog is validated as a whole: a profile naming an unknown model fails when it is loaded, not when an engine starts.
  - whisper.cpp is not in the image yet. Transcription arrives in M1 (plan §4.4: upstream `whisper-cli`), and joins the image then.
- **The checksums are not yet verified against the Hub.** They come from LocalAI's model gallery. `scripts/model-hashes.py check` compares them with Hugging Face's own records, and `pin` fixes each URL to a commit, so a later re-upload cannot change what a pod downloads. Its tests run against a fake Hub. Run it before the first sync: the runbook says how.

## The fake engine

`rr_fake_engine` speaks the part of llama-server's interface the supervisor and the hub use:
- `/health` answers 503 while "loading", then 200;
- `/props` reports the context per slot as llama-server computes it from `--parallel`, `--ctx-size` and `--kv-unified`;
- chat completions, JSON or SSE. A prompt of a slot's context or more is refused with `400 exceed_context_size_error`, before any token, as llama-server refuses it;
- embeddings;
- `/stats` counts the streams and requests its clients abandoned.

**Its streams are shaped as llama-server's** (checked against llama.cpp `db00347`, 2026-09-30): a role chunk whose content is null, `reasoning_content` then `content` deltas, a finish chunk with `"delta": {}`, the usage chunk (`"choices": []`) when `stream_options.include_usage` asks for it, `timings` on the last JSON chunk, and `[DONE]`. `tests/fake_engine/include/rr/fake_engine/stream.hpp` builds them; the fake and the tests that check what arrives share it, so they cannot drift apart.

**Process flags** script its life: `--fake-load-ms`, `--fake-never-ready`, `--fake-exit-after-ms`, `--fake-token-ms`. And its context:
- `--fake-slot-context N` makes `/props` report N, whatever the arithmetic, as when llama.cpp caps a slot at the model's training context;
- `--fake-no-props` removes `/props`, and `--fake-props-ms N` delays its answer;
- `--fake-prompt-cache` reuses prompt prefixes, as llama-server does by default. A prompt's longest common prefix with the last 16 it read counts as cached (`usage.prompt_tokens_details.cached_tokens`, `timings.cache_n`), but at least its last token is read again; `"cache_prompt": false` opts a request out. It is off unless asked for, so other tests see no cache.

**Knobs in a request's JSON body** script one call:

| Knob | What it simulates |
|---|---|
| `head_delay_ms` | A queued request |
| `first_token_ms` | Prompt reading |
| `break_after` / `break_mid_event` | A crash mid-answer |
| `error_after` | llama-server's error event mid-answer, then an end with no `[DONE]` |
| `split_pause_ms` | A pause inside an event |
| `fail` | A 400 |
| `echo_headers` | Reports the headers it received |
| `reply`, `reply_by_turn` | A given answer; `reply_by_turn` picks one per earlier assistant turn (a model repairing its answer) |
| `reasoning` | Thinking before the answer |
| `max_tokens` | The OpenAI field: at most that many pieces (`length` when the answer is cut) |
| `ignore_eos` | llama-server's field: with no `reply`, the answer runs to exactly `max_tokens` tokens, as `rreport bench` asks. Without it the answer ends by itself after five tokens, so a benchmark that forgot it would be caught |
| `n`, `replies` | The OpenAI field: n choices (at most 16), streamed as llama-server streams them. Each choice's role chunk comes first, the choices' tokens interleave, then each choice's finish and usage, written together; every choice reports the whole prompt. `replies[i]` is choice i's answer; by default every choice gives the same one |

It probes its client every 10 ms while silent, as llama-server does, and exits when its parent goes away, so a crashed test leaves no orphans.

**Ports.** Tests draw engine ports at random from 20000–32000, below Linux's ephemeral range (32768–60999). A port picked from the ephemeral range could be taken, between the probe and the engine's `bind()`, as the local port of one of the test's own client connections.

## The real engine, nightly (L4)

The fake engine is only as faithful as what it was told. Once a night, `.github/workflows/nightly.yml` checks it against the real thing: llama-server built for the CPU at the image's pinned llama.cpp (the same tag and commit as `docker/runtime/Dockerfile`), behind a real `rreport-runtime`, called through the hub's own client (`tests/live/live_test.cpp`).

**The model is made, not downloaded.** `scripts/tiny-model.py` writes a 25 MB GGUF in two seconds, in plain Python:
- llama.cpp's `qwen3` architecture at toy size (2 layers, width 64, grouped-query attention, per-head norms);
- Qwen's tokenizer, copied from the vocabulary-only file llama.cpp keeps for its tokenizer tests, with `<think>` and `</think>` restored at Qwen3's ids;
- Qwen3.5's chat template, which opens the thinking block in the prompt, so a thinking budget can be checked with a model that would never open one;
- weights drawn from a seed: the same seed writes the same bytes.

Its weights are random, so what it says is noise. That is enough, because what the tests check is the server's mechanics, not a model's skill:

| Check | What it confirms |
|---|---|
| Each engine's context per slot | An even split, a shared pool, and the model's trained context capping the catalog's figure, all as `/props` reports them and the runtime relays them |
| A grammar keeps the fields in the order written | P1's ordered schemas reach llama.cpp's grammar intact |
| Several answers report their own usage | P3's per-choice parsing, against llama-server's own stream |
| A thinking budget ends the thinking | With a budget the model answers; without one it thinks to the end |
| A shared system message is served from the cache | The prompt layout the pipeline relies on |
| A prompt beyond a slot's share is refused before it starts | The fake's `400 exceed_context_size_error` |
| Requests that overflow a shared pool all fail, and the engine carries on | The claim behind the army's token budget, and runbook R1's long-prompt step |

Without `RR_LIVE_LLAMA_SERVER` and `RR_LIVE_MODEL`, every live test skips itself. The suite is built and run in every CI job, so it cannot rot unnoticed. The job also runs whenever the suite itself changes (its workflow, `tiny-model.py`, `tests/live/`), so a change to it is checked before anything relies on it.

**Run it on your own machine:**

```sh
git clone --depth 1 --branch v0.5.0 https://github.com/ggml-org/llama.cpp ~/llama.cpp
cmake -S ~/llama.cpp -B ~/llama.cpp/build -DCMAKE_BUILD_TYPE=Release -DGGML_NATIVE=OFF
cmake --build ~/llama.cpp/build --target llama-server -j
scripts/tiny-model.py --llama-cpp ~/llama.cpp --out /tmp/tiny-qwen3.gguf
RR_LIVE_LLAMA_SERVER=~/llama.cpp/build/bin/llama-server RR_LIVE_MODEL=/tmp/tiny-qwen3.gguf \
  build/dev/tests/live/rr_live_tests
```

Each test starts a runtime of its own, three engines of the tiny model, in a second or two; the suite takes about 20 seconds on four cores.

## How the tests were validated

Each mechanism was removed on purpose, and the named end-to-end test was confirmed to fail. (Mutants are applied by pattern, and a pattern must match exactly once.)

| Mechanism removed | Caught by |
|---|---|
| Token check | `OnlyPingIsOpenEverythingElseNeedsTheToken` |
| Case-insensitive scheme | `OnlyPingIsOpenEverythingElseNeedsTheToken` |
| Boolean check of `"stream"` | `AnswersForUnknownModelsBadRequestsAndLoadingEngines` |
| Health checks are not activity | `HeartbeatsKeepThePodAliveAndIdlenessStopsIt` |
| Heartbeats are activity | `HeartbeatsKeepThePodAliveAndIdlenessStopsIt` |
| In-flight requests are never idle | `AStreamInFlightKeepsThePodAwake` |
| Upload verified against its name | `BlobUploadsAreVerifiedAndCheckable` |
| Engine restarts | `ACrashedEngineIsRestarted` |
| Handler notices a departed client | `AClientThatLeavesBeforeTheEngineAnswersCancelsIt` |
| Probing the client during silences | `AClientThatLeavesWhileTheEngineReadsCancelsIt` |
| Keep-alives | `KeepsASilentStreamAliveWithComments` |
| Keep-alives only at a line start | `KeepAlivesNeverSplitAnEvent` |
| The early `200` | `AnswersAtOnceWhenTheEngineHoldsItsHeaders` |
| The early `200` only for streamed calls | `NonStreamedCallsWaitForTheEngine` |
| Late errors in the stream | `ALateEngineErrorArrivesInTheStream` |
| Cutting a broken answer short | `AStreamThatBreaksOffLooksBroken` |
| Error events only between events | `ABreakInsideAnEventLeavesNoGarbage` |
| Shutdown ends waiting relays | `StoppingEndsWaitsForSlowEnginesPromptly` |
| Header allowlist | `ForwardsOnlyContentTypeAndAccept` |
| Re-hashing a partial before resuming | `ResumesWhereAPartialDownloadEnded` |
| Publishing only verified content | `NeverPublishesContentWithAnotherHash` |
| One fresh start after a mismatch | `NeverPublishesContentWithAnotherHash` |
| Token only for the Hub's hosts | `SendsTheTokenOnlyToTheHub` |
| Progress resets the retry budget | `KeepsGoingWhileEachAttemptMakesProgress` |
| A 206 must start where we asked | `AResumeThatStartsElsewhereIsNeverAppended` |
| ctime in the verification record | `AnyChangeToTheFileInvalidatesTheRecord` |
| Declining compression | `DeclinesCompressionSoRangesMeanStoredBytes` |
| Free-space check | `ChecksFreeSpaceBeforeWriting` |
| Starting over when the range is ignored | `StartsOverWhenTheServerIgnoresTheRange` |
| Refusing a file that changed while hashed | `AFileThatChangesWhileHashedIsNotTrusted` |
| Termination signals blocked before any thread | `ServesUntilSigtermThenStopsItsEngines` |
| Verifying model files before serving | `RefusesToServeWrongModelFiles` |
| Global options after the command (`fallthrough`) | `CatchesMisspeltSettings` |
| Environment aliases | `TheBuiltInDefaultsAreAWorkingConfiguration` |
| Conflicting aliases refused | `AliasesAgreeWithTheirCanonicalNamesOrFail` |
| `auto` sizes for the smallest GPU | `AutoPicksWhatTheSmallestGpuHolds` |
| On RunPod, an idle pod stops itself through RunPod's API | `OnRunpodAnIdlePodStopsItselfThroughTheApi` |
| The supervisor calls it, and again if the pod is still up | `WithoutACommandAnIdlePodStopsItselfAndRetries` |
| A configured stop command wins | `AKeyOrACommandOfOnesOwnWins` |
| The operator's key wins over the pod's own | `AKeyOrACommandOfOnesOwnWins` |
| Without a key, idleness is only logged | `AKeyOrACommandOfOnesOwnWins` |
| `"0s"` turns a timer off | `ZeroTurnsIdleStopAndKeepAlivesOff` |
| The catalog refuses the supervisor's own flags in `extra_args` | `RefusesInvalidDefinitions` |
| …and reads `--ctx_size` as `--ctx-size`, as llama.cpp does | `RefusesInvalidDefinitions` |
| The catalog refuses unknown keys | `RefusesInvalidDefinitions` |
| …and wrong types, rather than using the default (integers, booleans, strings) | `RefusesInvalidDefinitions` |
| …and arrays that are not of tables | `RefusesInvalidDefinitions` (by a crash, without the check) |
| …and a context that does not fit, a negative `min_vram_mib`, a projector that is not a table | `RefusesInvalidDefinitions` |
| `kv_unified` is read | `ParsesModelsAndProfiles`, and five other tests |
| A shared pool gives one sequence the whole context | `ContextPerSlotIsAShareOrTheWholePool` |
| The pool is stated on the command line, either way | `ListensOnLocalhostAndCarriesTheProfile` |
| The engine's own figure for a slot's context wins | `EnginesReportTheContextTheirSlotsReallyHave` |
| …it is read before the engine is declared ready (its `/props` answers slowly in the test) | `EnginesReportTheContextTheirSlotsReallyHave` |
| …a figure of 0 or less is refused | `EnginesReportTheContextTheirSlotsReallyHave` |
| …a discrepancy, and a `/props` that fails, are logged plainly | `EnginesReportTheContextTheirSlotsReallyHave` |
| Each engine reports its context, its pool and its model file's checksum | `EnginesReportTheContextTheirSlotsReallyHave` |
| The health report writes, reads and strictly types the new fields | `RoundTripsThroughJson`, `ParsingIsStrictAndNamesThePath` |
| The protocol is version 2 | `RefusesAnotherProtocolVersion` |

The mutation runs found four things, all corrected before they counted:
- **`in-flight-ignored` removed one of two copies of the rule.** Removing only `stops_in()`'s in-flight check would have "survived" because `idle_for()` also reports zero while a request runs.
- **`shutdown-ignored` would be caught by a different test than listed.** A stream producer also ends through `ChunkWriter::stopping()` when the server stops. Only a handler waiting for an engine's head depends on the shutdown token alone.
- **Removing the probe survived at first: a weak test, not a useless probe.**
  - With the tests' 100 ms keep-alive, the keep-alive writes noticed the departed client by themselves.
  - With production's 15 s keep-alive, only the probe notices within a tick, instead of up to 15 s of GPU time later.
  - The test now runs with a 10 s keep-alive and requires the engine to be freed within 2 s.
- **`CatchesMisspeltSettings` passed without `fallthrough`, for the wrong reason.**
  - Without it, the argument parser refuses the `--set` after `serve`. Its error echoes the command line, which also holds `server.port=0`, so looking for "server.port" matched anyway.
  - The test now requires the configuration loader's own words: the key it did not know, and its suggestion.

For engines' context (P5), writing the mutants showed two gaps, and running them a third:
- **Reading the figure after the engine was declared ready would have passed** (seen while writing). The fake answered `/props` at once, so no client could catch the engine ready with the catalog's figure. The test's engine now answers slowly (`--fake-props-ms`).
- **A figure of 0 had no test** (seen while writing). The `odd` profile's engine now reports one.
- **Ignoring `/props`' status survived the run.** A 404 fails anyway, since its body is not `/props`' answer, so the check's only effect is the warning's wording: "/props answered 404" rather than a parse error. That wording is what an operator reads, so the test now captures the warnings and requires each one.

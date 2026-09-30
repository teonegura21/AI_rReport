# The pod runtime: engines, the relay, blobs and idle stop

`rreport-runtime` runs on the rented GPU pod. It is the pod's one HTTP door, and everything behind that door is either its own code or a child process it manages. The code is small, but each rule below exists because of how GPUs, llama-server and RunPod's proxy actually behave. They are recorded here so nobody "simplifies" one away.

## Pieces

| Where | What it is |
|---|---|
| `src/runtime_api/` (`rr_runtime_api`) | The hub ↔ runtime protocol v1: endpoint paths, the health report and its strict JSON form, and library-name validation. Linked by both sides |
| `src/supervisor/catalog.*` | The model catalog (TOML): models with file, sha256 and URL, and GPU profiles that say which engines run with which slots and context |
| `src/supervisor/gpu.*` | `nvidia-smi` CSV parsing: name, memory, utilization, driver |
| `src/supervisor/engine.*` | One engine = one `llama-server` child process, kept alive (lifecycle below) |
| `src/supervisor/models.*` | Model files: verified against the catalog's sha256 (with a safe cache), downloaded and resumed when missing |
| `src/supervisor/proxy.*` | The relay: an OpenAI-compatible call in, the engine's answer streamed out (ADR-0014) |
| `src/supervisor/watchdog.*` | When the pod should stop itself |
| `src/supervisor/supervisor.*` | The wiring: routes, the auth guard, blobs, engines, and the watcher thread |
| `tests/fake_engine/` | `rr_fake_engine`: a scriptable llama-server stand-in, so every behaviour is tested end to end without a GPU |

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
- **The stop.** After `idle_stop` of idleness the watcher runs the stop command once, and retries it after 5 minutes if the pod is still up.
  - RunPod's documented form is `runpodctl stop pod $RUNPOD_POD_ID`. It is legacy but still supported by the current CLI, whose own form is `runpodctl pod stop <id>`, and it is the form the older runpodctl copy inside pods understands.
  - Pods ship runpodctl with a pod-scoped API key. Which forms that copy accepts must be checked on a real pod (M0).
- **No stop command configured.** The runtime only logs, loudly.

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

## The fake engine

`rr_fake_engine` speaks the part of llama-server's interface the supervisor uses:
- `/health` answers 503 while "loading", then 200;
- chat completions, JSON or SSE;
- embeddings;
- `/stats` counts the streams and requests its clients abandoned.

**Process flags** script its life: `--fake-load-ms`, `--fake-never-ready`, `--fake-exit-after-ms`, `--fake-token-ms`.

**Knobs in a request's JSON body** script one call:

| Knob | What it simulates |
|---|---|
| `head_delay_ms` | A queued request |
| `first_token_ms` | Prompt reading |
| `break_after` / `break_mid_event` | A crash mid-answer |
| `split_pause_ms` | A pause inside an event |
| `fail` | A 400 |
| `echo_headers` | Reports the headers it received |

It probes its client every 10 ms while silent, as llama-server does, and exits when its parent goes away, so a crashed test leaves no orphans.

**Ports.** Tests draw engine ports at random from 20000–32000, below Linux's ephemeral range (32768–60999). A port picked from the ephemeral range could be taken, between the probe and the engine's `bind()`, as the local port of one of the test's own client connections.

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

The mutation run found three things, all corrected before it counted:
- **`in-flight-ignored` removed one of two copies of the rule.** Removing only `stops_in()`'s in-flight check would have "survived" because `idle_for()` also reports zero while a request runs.
- **`shutdown-ignored` would be caught by a different test than listed.** A stream producer also ends through `ChunkWriter::stopping()` when the server stops. Only a handler waiting for an engine's head depends on the shutdown token alone.
- **Removing the probe survived at first: a weak test, not a useless probe.**
  - With the tests' 100 ms keep-alive, the keep-alive writes noticed the departed client by themselves.
  - With production's 15 s keep-alive, only the probe notices within a tick, instead of up to 15 s of GPU time later.
  - The test now runs with a 10 s keep-alive and requires the engine to be freed within 2 s.

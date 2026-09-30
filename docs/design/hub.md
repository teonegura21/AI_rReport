# The hub's command line: `rreport`

`rreport` runs on your own machine. It owns every piece of state (ADR-0001), captures videos, and sends the GPU work to the pod. In M0 it carries the operator's tools: the checks, the pod's controls and the benchmark. This note describes how they work and why. The pod's side is described in [runtime.md](runtime.md); the model calls in [llm.md](llm.md).

## Pieces

| Where | What |
|---|---|
| `apps/rreport/main.cpp` | The executable: it parses arguments and wires modules together |
| `src/hub/…/hub_config.hpp` | From configuration to `HubSettings`, with the rules unit-tested |
| `src/hub/…/doctor.hpp` | The checks, as data (findings) |
| `src/hub/…/bench.hpp` | The benchmark |
| `src/runtime_client/…/pod_manager.hpp` | Starting the pod and waiting until its engines are ready |
| `src/runpod/…/runpod.hpp` | RunPod's pod API |
| `config/default.toml` | The built-in defaults, which are also the schema |

## Commands

| Command | What it does | Exit codes |
|---|---|---|
| `doctor` | Checks the data directory, the library, the capture tools, RunPod and the runtime, and says what to do about each problem | 0 no failures (warnings allowed); 1 a failure |
| `bench [options]` | Starts the pod if it is stopped, waits for its engines, and measures them (see below) | 0; 1 on failure |
| `pod status` | The pod as RunPod reports it, and each engine's state | 0; 1 on failure |
| `pod start [--no-wait]` | Starts the pod and waits until every engine is ready | 0; 1 on failure |
| `pod stop` | Stops the pod; its `/workspace` volume is kept | 0; 1 on failure |
| `config show [--origin]` | Prints every setting, secrets masked, optionally with where each value came from | 0 |
| `version` | Prints the version | 0 |

**For every command:** 2 means a usage or configuration error, and 128+N means signal N interrupted the command.

## Configuration

**Layers.** Each layer overrides the ones before (rr_config):
1. the built-in defaults;
2. the config file: `--config`, else `RREPORT_CONFIG` (both must exist), else `$XDG_CONFIG_HOME/rreport/config.toml` (default `~/.config/rreport/`) when it exists;
3. the secrets file beside it (`secrets.toml`, which must be `chmod 600`);
4. `RREPORT_<SECTION>__<KEY>` environment variables;
5. `--set section.key=value`.

A misspelt key or a wrong type fails with a suggestion.

**Resolution rules** (`hub_settings_from_config()`):
- **Data directory:** `hub.data_dir`, else `$XDG_DATA_HOME/rreport`, else `~/.local/share/rreport`. Relative XDG paths are ignored, as the specification requires. Each library lives in `<data_dir>/libraries/<name>/library.db`.
- **The runtime's address:** `runtime.url`, else RunPod's proxy URL for `runpod.pod_id` and `runpod.port` (`https://<pod>-8080.proxy.runpod.net`). Without either, the hub works but cannot call models.
- **`llm.mode`:** `live`; `record` (live, keeping every answer); or `replay` (recorded answers only, for tests).
- **Secrets:** `runtime.token` (the pod's `RR_RUNTIME_TOKEN`) and `runpod.api_key` (an API key of your RunPod account). They belong in `secrets.toml` or the environment, and are never printed.

## `doctor`

The checks produce findings: what was found, how bad it is, and what to do. The command only prints them, so the rules are tested without a terminal. The checks run in order:

1. **Data directory.** It must be writable (a probe file is written atomically, then removed). Less than 20 GiB free is a warning: audio, frames and intermediate work need room.
2. **Library.** It is opened, and created and migrated on first use. The schema version is reported. SQLite without FTS5 is a warning, because search needs it.
3. **Capture tools.** `yt-dlp`, `deno` (yt-dlp runs YouTube's JavaScript challenges with it), `ffmpeg` and `ffprobe`. Each missing one is a warning with its install hint. Capture arrives in M1.
4. **RunPod.**
   - Checks the key and the pod: `pod abc123 is RUNNING: 1 × RTX 4090, $0.44/h`.
   - A refused key and an unknown pod get different advice.
   - An unconfigured pod is a warning: the hub works without one.
5. **Runtime.**
   - Checks the token, then the protocol version and the engines.
   - A stopped pod's runtime is not asked for at all. The advice is `rreport pod start`, instead of a confusing connection error.
   - Engines still loading are a warning.
   - A hub and a runtime from different releases get a `versions` warning. Both name their release the same way (`kFullVersion`), so the comparison is exact.

**Severity.** A failure means the hub cannot do its work until it is fixed. A warning means something will not work yet, or is worth a look.

## The pod: start, wait, stop

`PodManager` combines RunPod's API with the runtime's health report.

**Starting (`ensure_ready()`).** It acts on the pod's status:
- **RUNNING:** wait for the engines.
- **EXITED or ERROR:** start it first.
- **PROVISIONING or STARTING:** wait.
- **TERMINATED:** fail; that pod cannot come back.

Once RunPod reports RUNNING, `RuntimeClient::wait_until_ready()` polls the health report until every engine is ready. While it waits:
- a pod that has no route yet (the proxy's `404` or `502`) or is restarting an engine counts as still booting;
- a wrong token or another protocol version ends the wait at once;
- a pod that falls back to EXITED or ERROR ends the wait, after a 60-second grace period in which RunPod may still report the status the pod was started from.

**Facts from RunPod's API** (REST v2, `api.runpod.io/v2`, researched 2026-09-30), and what the hub does with each:

| Fact | What the hub does |
|---|---|
| `start` and `stop` are `POST /pods/{id}/action`; an action the status does not allow answers `409` | A conflict is resolved by reading the pod again |
| A stopped pod resumes on its own machine only; when that machine's GPUs are taken, the start fails (`400`, "not enough free GPUs") | Reported at once as `unavailable`, with RunPod's advice: wait, or create a new pod on the same network volume. Not retried for hours |
| Stopping keeps the `/workspace` volume and clears the container disk | Models and blobs live under `/workspace` |
| `429` carries `Retry-After` in seconds | Honoured by the retry policy |
| REST v1 retires on 2026-11-15 | v1 is not used |

**Stopping** (`stop_pod()`) stops a pod that is up or coming up, and leaves a stopped one alone. The pod also stops itself once idle (ADR-0015).

## `bench`: what a pod does per second and per dollar

The GPU is the product's main running cost. Which one to rent is decided by measurement (plan §4.5): run `rreport bench` on a pod of each candidate type, and compare the tables.

**Method.**
- **The commander** is measured one request at a time, `--rounds` times (its passes are sequential): time to the first token, then the prompt and generation speeds that the engine itself reports in `timings`.
- **The infantry** is measured at 1, 4, 8, 16 and 32 requests at once (`--parallel`). The squad's worth is its **total** tokens per second. Levels above the engine's `slots` are marked "queued", because llama-server makes extra requests wait for a slot.
- **Peak VRAM** is sampled from the runtime's health report every second while the load runs.
- **Cold start (`--cold-start`).** The pod is stopped first, and timed from the start until every engine is ready. It includes the image start and model loading from the network volume.
- **Cost** is dollars per million generated tokens: `price per hour / (tokens per second × 3600) × 10⁶`, at RunPod's price for the pod, or `--price-per-hour`.

**Fair comparisons.**
- Every request asks for exactly `--output-tokens` tokens (`ignore_eos`), at temperature 0 with a fixed seed, with thinking off.
- Prompts are read in full (`cache_prompt: false`), and each parallel request starts differently, so no two share a cached prefix.
- A failed request counts in the table's `failed` column. A level where every request failed ends the run with the error.

**Results** go to stdout as Markdown, and to `<data_dir>/bench/<time>.json` (or `--out`). Every call is recorded in the library's `llm_calls` with the purpose `bench`.

## How it is tested

| Level | What |
|---|---|
| Unit | `hub_config_test.cpp`: XDG rules, file locations, the runtime's address, mode and limit checks. `doctor_test.cpp`: each finding, against a hand-written runtime and the fake RunPod API |
| Unit, RunPod and the pod | `runpod_test.cpp`: the API against `tests/fakes/fake_runpod.hpp`, a fake RunPod that refuses what RunPod refuses (409, 400 for a full machine, 429, 403 for a pod's own key on REST) and whose reads can lag its writes. `pod_manager_test.cpp`: that fake plus a real runtime with fake engines. `runtime_client_test.cpp`: the proxy's 404 and 502 while a pod boots, served by hand |
| Component | `tests/component/hub_cli_test.cpp` runs the real `rreport` against a real `rreport-runtime` (fake engines: a commander, and infantry with 4 slots) and the fake RunPod API. It covers `doctor`'s verdicts and advice; `pod start`, `status` and `stop`; a full `bench`, with the queued level marked, RunPod's price, the JSON saved and 11 calls accounted for; a timed cold start; and configuration mistakes, with secrets never printed |

### How the tests were validated

Each rule was removed on purpose, and the named test was confirmed to fail.

| Rule removed | Caught by |
|---|---|
| Relative XDG paths are ignored | `TheDataDirectoryFollowsXdg` |
| The runtime's address follows the pod | `TheRuntimeFollowsThePod` |
| A config file named with `--config` must exist | `ConfigurationFilesAreFoundWhereTheyLive` |
| A stopped pod's runtime is not asked for | `AStoppedPodsRuntimeIsNotAskedFor` |
| Little room is a warning | `MissingToolsAndLittleRoomAreWarnings` |
| A failure makes the hub unhealthy | `AnUnusableDataDirectoryIsAFailure` |
| A version mismatch is reported | `ChecksTheRuntime` |
| Only a booting pod is waited for: a wrong token ends the wait | `AWrongTokenEndsTheWaitAtOnce` |
| The proxy's 404 and 502 are waited out | `APodStillBootingBehindRunpodsProxyIsWaitedFor` |
| `/ping` must answer `ok` | `RefusesARuntimeOfAnotherProtocol` |
| The provider is named `runtime` | `ChatsWithTheEnginesThroughTheRelay` |
| The start's grace period | `APodStillReportedStoppedAfterItsStartIsWaitedFor` |
| The grace covers only the status the pod was started from | `APodThatFallsBackEndsTheWait` |
| A pod that falls back ends the wait | `APodThatFallsBackEndsTheWait` |
| A terminated pod is refused | `APodThatFallsBackEndsTheWait` |
| A start refused as a conflict re-reads the pod | `APodSomeoneElseStartedMeanwhileIsWaitedFor` |
| A stopped pod is not stopped again | `StopsAPodThatIsUpAndLeavesAStoppedOneAlone` |
| A cold start is reported | `StartsAStoppedPodAndWaitsForItsEngines` |
| GraphQL only when REST refuses the key | `APodStopsItselfEvenWhenRestRefusesItsKey`, `AKeyRestAcceptsNeedsNoGraphql` |
| A full machine is explained | `AFullMachineIsExplained` |
| `Retry-After` is honoured | `ThrottlingCarriesTheServersDelay` |
| Pod IDs are checked before use | `RefusalsKeepRunpodsExplanation` |
| Levels above the slots are marked "queued" | `BenchMeasuresThePodAndKeepsTheResults` |
| Every answer runs to `--output-tokens` (`ignore_eos`) | `BenchMeasuresThePodAndKeepsTheResults` |
| Every bench call is accounted for | `BenchMeasuresThePodAndKeepsTheResults` |
| RunPod's price is used | `BenchMeasuresThePodAndKeepsTheResults` |
| `--cold-start` stops the pod first | `BenchTimesAColdStart` |
| The cold start is timed | `BenchTimesAColdStart` |

**What the runs found, all fixed before they counted:**
- **The fakes were too kind in three places:**
  - The fake engine ran every answer to `max_tokens`, as if `ignore_eos` were always on, so a bench that forgot `ignore_eos` measured the same. Now an answer ends by itself after five tokens unless `ignore_eos` is set.
  - The fake RunPod counted only the actions it accepted, so a second, pointless stop (refused with `409`) went unseen. It now counts every action asked for.
  - The fake RunPod's reads never lagged a start, so the grace period never ran. `set_start_lag()` and `serve_stale()` now make reads trail writes, as RunPod's may.
- **Two paths had no test at all:** the proxy's 404 while a pod boots, and a start refused because another hub had just started the pod. Each has one now.
- **A named config file could be silently optional.** The test checked the path the flag gave, but not that the file had to exist.

# The hub's command line: `rreport`

`rreport` runs on your own machine. It owns every piece of state (ADR-0001), captures videos, and sends the GPU work to the pod. It carries the operator's tools (the checks, the pod's controls and the benchmark) and, since P15, jobs: `ingest`, `resume` and `jobs`, whose stages are described in [pipeline.md](pipeline.md). This note describes how the tools work and why. The pod's side is described in [runtime.md](runtime.md); the model calls in [llm.md](llm.md).

## Pieces

| Where | What |
|---|---|
| `apps/rreport/main.cpp` | The executable: it parses arguments and wires modules together |
| `src/hub/…/hub_config.hpp` | From configuration to `HubSettings`, with the rules unit-tested |
| `src/hub/…/doctor.hpp` | The checks, as data (findings) |
| `src/hub/…/bench.hpp` | The benchmark |
| `src/hub/…/model_stack.hpp` | The providers every model call goes through: built against the runtime only (ADR-0017), wired by `llm.mode` ([llm.md](llm.md#a-call-as-it-is-wired)) |
| `src/hub/…/army_engines.hpp` | The army's engines, from the pod's health report and each family's sampling presets ([agents.md](agents.md#engines-and-roles)) |
| `src/hub/…/job_army.hpp` | A job's army, raised the first time a stage needs a model: the pod, its engines, the job's memo over the model stack, the battle log ([pipeline.md](pipeline.md#the-army-raised-when-needed)) |
| `src/runtime_client/…/pod_manager.hpp` | Starting the pod and waiting until its engines are ready |
| `src/runpod/…/runpod.hpp` | RunPod's pod API |
| `config/default.toml` | The built-in defaults, which are also the schema |

## Commands

| Command | What it does | Exit codes |
|---|---|---|
| `ingest SOURCE [--tier] [--salt]` | A new job for a video's address or a caption file, through the stages so far ([pipeline.md](pipeline.md#the-commands)) | 0 the job is done; 1 it is not (failed, stopped, needing you) |
| `resume JOB` | The job taken up again where it stopped | as `ingest` |
| `jobs [--limit N]`, `jobs show JOB` | The library's jobs, newest first; one job's stages and findings | 0; 1 on failure |
| `doctor` | Checks the data directory, the library, the capture tools, RunPod and the runtime, says where model calls go (`models`), and what to do about each problem | 0 no failures (warnings allowed); 1 a failure |
| `bench [options]` | Starts the pod if it is stopped, waits for its engines, and measures them (see below) | 0; 1 on failure |
| `pod status` | The pod as RunPod reports it; then, from the runtime, its profile, its GPUs (VRAM total and used) and each engine's state | 0; 1 on failure |
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
- **yt-dlp:** `media.ytdlp`, a name found in `PATH` or a path, and `media.ytdlp_timeout`, the most one run of it may take. `doctor` checks the one configured.
- **Sampling presets:** every `[sampling.<name>]` table (`qwen_thinking`, `qwen_direct`, `gemma_thinking`, `gemma_direct`), with every field required. Each is checked when the configuration loads (`llm::preset_problems()`), so a bad value fails at startup, not in the middle of a job. Values and reasons: [llm.md](llm.md#sampling).
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
- **The engines** are listed as the runtime reports them: slots, context, the most one sequence may hold (`per slot`), and whether the slots share one pool. The pipeline sizes its work by these figures ([runtime.md](runtime.md#context-per-slot-and-the-shared-pool)).
- **The commander** is measured one request at a time, `--rounds` times: time to the first token, then the prompt and generation speeds that the engine itself reports in `timings`.
- **Several answers at once (`--n K`).** Each commander request asks for K answers, as the pipeline asks for k drafts (ADR-0018). The prompt is read once and copied to K slots, and the generated speed is the answers' together.
  - K may not exceed the commander's slots, and the bench refuses it before any call: llama-server starts such a request only when K slots are free at once, so with fewer slots in all it would hold the request forever.
  - K answers need sampling (at temperature 0 they would all be the same answer, and such a request is refused), so these requests use temperature 1. Every answer still runs to exactly `--output-tokens` tokens.
- **The commander at several requests at once (`--commander-parallel 1,2,4`).** The pipeline sends the commander missions in parallel. Decoding is limited by memory bandwidth, so four streams should give about three times the tokens per second of one, and this table says how much they really give. The levels are marked "queued" above the engine's slots, as for the infantry.
- **The infantry** is measured at 1, 4, 8, 16 and 32 requests at once (`--parallel`). The squad's worth is its **total** tokens per second. Levels above the engine's `slots` are marked "queued", because llama-server makes extra requests wait for a slot.
- **The prompt cache (`--cache-check`).** The pipeline's prompts put everything shared (role, rules, the video's brief) in the system message and vary only the last user message, so that the engine can serve the shared part from its cache. This check tests that it does, for the commander and the infantry:
  1. one request reads a system message of about `--prompt-tokens` tokens, which nothing has sent before (it carries the run's random id);
  2. the next request sends the same system message with another question;
  3. on an engine with two slots or more, up to four requests do that at once.

  Each row reports the prompt's tokens and how many the engine served from its cache (`usage.prompt_tokens_details.cached_tokens`). A round of requests at once reports the least any of them had, since one that read the prefix again is the news. A row counts as **reused** when at least half its prompt came from the cache: the shared prefix is nearly the whole prompt, while a few tokens in common (the chat template's start) are not the prefix. A one-line verdict per engine says where reuse stopped: not at all, only one request at a time, or not even by requests at once. Hybrid models, Qwen3.8 among them, can reuse a prefix only from checkpoints, so this is a real question, answered on the pod.
- **Peak VRAM** is sampled from the runtime's health report every second while the load runs.
- **Cold start (`--cold-start`).** The pod is stopped first, and timed from the start until every engine is ready. It includes the image start and model loading from the network volume.
- **Cost** is dollars per million generated tokens: `price per hour / (tokens per second × 3600) × 10⁶`, at RunPod's price for the pod, or `--price-per-hour`.

**Fair comparisons.**
- Every speed request asks for exactly `--output-tokens` tokens (`ignore_eos`), at temperature 0 with a fixed seed, with thinking off.
- Speed prompts are read in full (`cache_prompt: false`), and each parallel request starts differently, so no two share a cached prefix. Only the cache check uses the cache.

**A limit found is a result.** A failed request counts in the table's `failed` column, and a note under the table gives the first failure's reason. A level where every request failed is kept too, with dashes for its speeds: a prompt beyond the engine's context per slot, or requests that together overflow a shared pool, are limits worth seeing (an overflow of a shared pool fails every request in flight, and llama-server carries on). Only an interruption, a failure of the commander's first measurement, or a failure of the cache check ends the run.

**Results** go to stdout as Markdown, and to `<data_dir>/bench/<time>.json` (or `--out`). Every call is recorded in the library's `llm_calls` with the purpose `bench`.

## How it is tested

| Level | What |
|---|---|
| Unit | `hub_config_test.cpp`: XDG rules, file locations, the runtime's address, mode and limit checks, sampling presets. `doctor_test.cpp`: each finding, against a hand-written runtime and the fake RunPod API |
| Unit, RunPod and the pod | `runpod_test.cpp`: the API against `tests/fakes/fake_runpod.hpp`, a fake RunPod that refuses what RunPod refuses (409, 400 for a full machine, 429, 403 for a pod's own key on REST) and whose reads can lag its writes. `pod_manager_test.cpp`: that fake plus a real runtime with fake engines. `runtime_client_test.cpp`: the proxy's 404 and 502 while a pod boots, served by hand |
| Unit, the bench | `bench_test.cpp`: how a round of cache-check requests is summed up (the least reuse wins), each verdict and the threshold at exactly half, and how a level that failed is reported, in Markdown and JSON |
| Component | `tests/component/hub_cli_test.cpp` runs the real `rreport` against a real `rreport-runtime` (fake engines: a commander with 2 slots sharing a pool of 4,096 tokens and a prompt cache, and infantry with 4 slots of 2,048 tokens each and none) and the fake RunPod API. It covers `doctor`'s verdicts and advice; `pod start`, `status` (with each engine's context) and `stop`; a full `bench`, with the queued level marked, RunPod's price, the JSON saved and 11 calls accounted for; the engines' table, the commander at 1, 2 and 3 requests at once, and the prompt cache reused by one engine and not by the other (18 calls); a level beyond the infantry's context per slot, kept as a result with its reason; a timed cold start; and configuration mistakes, with secrets never printed |

### How the tests were validated

Each rule was removed on purpose, and the named test was confirmed to fail.

| Rule removed | Caught by |
|---|---|
| Relative XDG paths are ignored | `TheDataDirectoryFollowsXdg` |
| The runtime's address follows the pod | `TheRuntimeFollowsThePod` |
| A config file named with `--config` must exist | `ConfigurationFilesAreFoundWhereTheyLive` |
| Presets are checked at startup | `ABadPresetFailsAtStartup` |
| A misspelt reasoning effort is refused, not dropped | `ABadPresetFailsAtStartup` |
| A thinking budget is a token count, or -1 | `ABadPresetFailsAtStartup` |
| Preset names come from the defaults' tables | `SamplingPresetsComeFromTheModelCards` |
| A stopped pod's runtime is not asked for | `AStoppedPodsRuntimeIsNotAskedFor` |
| Little room is a warning | `MissingToolsAndLittleRoomAreWarnings` |
| A failure makes the hub unhealthy | `AnUnusableDataDirectoryIsAFailure` |
| A version mismatch is reported | `ChecksTheRuntime` |
| `doctor` says where model calls go | `ChecksTheRuntime`, `AReplayCallsNoModel` |
| `ModelStack`: retries only when asked, above accounting | `TransientFailuresAreRetriedAndEachAttemptAccounted` |
| `ModelStack`: recording only in record mode, once per call | `RecordModeKeepsEachCallOnce`, `EveryLiveCallGoesToTheRuntimeAndIsAccounted` |
| `ModelStack`: replay calls no model | `AReplayCallsNoModelAndNeedsNoRuntime` |
| `bench` refuses replay | `ConfigurationMistakesAndSecrets` |
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
| `--n` above the commander's slots is refused before any call | `BenchAsksTheCommanderForSeveralAnswersAtOnce` |
| `--n` samples (temperature 1) | `BenchAsksTheCommanderForSeveralAnswersAtOnce` |
| RunPod's price is used | `BenchMeasuresThePodAndKeepsTheResults` |
| `--cold-start` stops the pod first | `BenchTimesAColdStart` |
| The cold start is timed | `BenchTimesAColdStart` |
| The engines are listed, with their context | `BenchWeighsTheCommandersSlotsAndThePromptCache` |
| `--commander-parallel` runs the commander at each level | `BenchWeighsTheCommandersSlotsAndThePromptCache` |
| `--cache-check` checks each engine | `BenchWeighsTheCommandersSlotsAndThePromptCache` |
| …with requests at once on engines with two slots or more | `BenchWeighsTheCommandersSlotsAndThePromptCache` |
| A round of requests at once reports the least reuse, and the largest prompt | `ARoundIsAsGoodAsItsLeastReusedRequest` |
| Reuse means at least half the prompt | `TheVerdictSaysWhereReuseStops` |
| Requests at once must reuse too | `TheVerdictSaysWhereReuseStops` |
| The verdict says where reuse stopped | `TheVerdictSaysWhereReuseStops` |
| A level that failed shows dashes and its reason, in Markdown and JSON | `ALevelThatFailedSaysWhy` |
| A level that failed is kept, and the run goes on | `BenchKeepsALevelTheEngineCannotServeAsAResult` |
| …with its reason | `BenchKeepsALevelTheEngineCannotServeAsAResult` |

**What the runs found, all fixed before they counted:**
- **The fakes were too kind in three places:**
  - The fake engine ran every answer to `max_tokens`, as if `ignore_eos` were always on, so a bench that forgot `ignore_eos` measured the same. Now an answer ends by itself after five tokens unless `ignore_eos` is set.
  - The fake RunPod counted only the actions it accepted, so a second, pointless stop (refused with `409`) went unseen. It now counts every action asked for.
  - The fake RunPod's reads never lagged a start, so the grace period never ran. `set_start_lag()` and `serve_stale()` now make reads trail writes, as RunPod's may.
- **Two paths had no test at all:** the proxy's 404 while a pod boots, and a start refused because another hub had just started the pod. Each has one now.
- **A named config file could be silently optional.** The test checked the path the flag gave, but not that the file had to exist.
- **Taking the most reuse in a round, not the least, could not be told apart end to end** (seen while writing the mutants). The fake engine's requests at once were all served alike. How a round is summed up, and the verdict's wording, became plain functions (`cache_row()`, `describe()`), tested in `bench_test.cpp` with rounds that differ.

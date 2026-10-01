# rReport architecture

This is the developer-facing map of the system. The product-level plan, including every decision's rationale, lives in [`plan.md`](plan.md). The individual decisions are recorded as [ADRs](adr/README.md).

## 1. The two programs

```
 YOUR LINUX LAPTOP (CPU)                                  GPU POD (RunPod; rented while working)
┌────────────────────────────────────────────┐  HTTPS  ┌─────────────────────────────────────────────┐
│ rreport — C++ hub (the source of truth)    │ bearer  │ rreport-runtime — CUDA-free supervisor      │
│  capture: yt-dlp(+Deno) · ffmpeg · frames  │ token   │  auth · blobs · jobs · OpenAI-compat proxy  │
│  pipeline (artifacts) · Fidelity Ledger    │────────▶│  VRAM policy · idle stop · dead-man switch  │
│  agents: missions · squads · dispatcher    │ blobs   │  engines = child processes on 127.0.0.1:    │
│  library: SQLite + FTS5 + vectors          │ jobs    │   llama-server  COMMANDER Qwen3.8-27B       │
│  retrieval · chat · research               │ chat    │   llama-server  INFANTRY  Gemma 4 × N slots │
│  PodManager (RunPod API)                   │◀────────│   llama-server  embedder · reranker         │
│  REST + SSE ─┬─ web UI (React + Vite)      │ (SSE)   │   rr-speech-worker  Parakeet·Whisper·diar.  │
│              └─ MCP adapter (TS)           │         └─────────────────────────────────────────────┘
└────────────────────────────────────────────┘
```

- **Hub (`apps/rreport`).**
  - Owns all state and runs the pipeline.
  - Talks to YouTube only from the user's own network.
  - Needs no GPU.
- **Runtime (`apps/rreport-runtime`).**
  - A stateless supervisor that serves models over one authenticated API.
  - Every GPU engine is a child process (ADR-0003).
- **Edges (TypeScript).** Web UI, browser extension and MCP adapter. They call the hub's REST/SSE API and hold no business logic (ADR-0002).

## 2. Source layout

```
src/<module>/include/rr/<module>/*.hpp   public headers of a module (the only includable surface)
src/<module>/*.cpp                        implementation (third-party types stay in here)
apps/<binary>/                            executables: thin mains wiring modules together
tests/unit/<module>/                      GoogleTest suites, one executable per module
tests/component/ tests/live/              the executables from outside; against a real llama-server
cmake/                                    build helpers (warnings, sanitizers, modules, deps, embed)
config/ migrations/ prompts/ schemas/     data files compiled into the binaries
docker/ deploy/                           runtime image and pod runbooks
docs/                                     plan, ADRs, design notes, runbooks
```

**Layering is enforced by the compiler.** `rr_add_module()` gives each module only its own `include/` directory, plus the include directories of the modules it *links*. A header from an unrelated module simply cannot be found.

## 3. Module map

Modules marked *(M0)* exist now; the rest arrive in the milestone shown.

| Module | Responsibility | Depends on |
|---|---|---|
| `rr_core` *(M0)* | `Result`/`Error`, strong IDs + ULIDs, time spans + clocks, BLAKE3/SHA-256, `Secret`, `RR_ASSERT`, `ThreadPool`, `Limiter` (priorities; permits of several slots at once), careful POSIX file I/O (EINTR, short writes, fsync of files and directories, whole files replaced atomically) | std, BLAKE3, OpenSSL (crypto) |
| `rr_log` *(M0)* | Structured logging (console + rotating JSONL) with per-thread context | core, nlohmann_json |
| `rr_config` *(M0)* | Layered TOML config with value origins and secrets | core, toml++ |
| `rr_json` *(M0)* | JSON parse/dump, canonical form, JSON-Schema validation, portable-schema lint; schemas keep their field order for models | core, nlohmann_json, jsoncons |
| `rr_db` *(M0)* | SQLite wrapper (WAL, writer + reader pool), migrations ([design](design/database.md)) | core, SQLite |
| `rr_blob` *(M0)* | Content-addressed, crash-safe blob store (BLAKE3 names, atomic publish) | core |
| `rr_proc` *(M0)* | Subprocesses: run-to-completion and supervised children, each in its own process group (ADR-0013); termination signals as a stop request | core (posix_spawn) |
| `rr_net` *(M0)* | HTTP client (libcurl), SSE parser, retry policy, HTTP server (cpp-httplib) ([design](design/networking.md)) | core, json, libcurl, cpp-httplib |
| `rr_runtime_api` *(M0)* | Hub↔runtime protocol v2: paths, the health report (each engine's context and model checksum), the streamed-call contract | core, json |
| `rr_runpod` *(M0)* | RunPod's pod API (REST v2): read, start and stop a pod; a pod stopping itself (ADR-0015) | core, net, json |
| `rr_supervisor` *(M0)* | Runtime: auth, health, engines, the streaming relay (ADR-0014), model files (verified, resumable downloads), blobs, idle/dead-man stop through RunPod's API (ADR-0015), the runtime's configuration ([design](design/runtime.md)) | runtime_api, net, proc, blob, config, runpod, json, log, toml++ |
| `rr_llm` *(M0)* | Calling models: one chat vocabulary (n choices, sampling presets), the OpenAI-compatible streaming provider, retries, accounting, record/replay, schema-validated answers with ordered fields ([design](design/llm.md)) | core, json, net, log |
| `rr_prompt` *(M1)* | The prompts: inja templates with numbered, hash-checked versions; the cache-aware layout (the system message from shared variables only, checked on the syntax tree); strict rendering; token estimates calibrated from past calls ([design](design/prompts.md)) | core, json |
| `rr_agents` *(M1)* | The army (ADR-0011): missions on the pod's engines, each engine's battalion within its slots and its shared pool's tokens; a mission's samples from one reading of its prompt; checks, repairs and the escalation ladder (thinking, the commander, another family after a refusal); the job's ceilings (ADR-0018); the battle log ([design](design/agents.md)) | core, json, llm, prompt, log |
| `rr_reason` *(M1)* | Many answers, one decision: votes per item, with waves of samples only where they disagree; the medoid; boundary consensus; pairwise judgements counted only when they survive a swap; lexicographic selection. And the tiers fast, thorough and max, strictly read and canonical for cache keys ([design](design/reasoning.md)) | core, json, toml++ |
| `rr_library` *(M0)* | A library's database (ADR-0008): its schema (`migrations/`, compiled in) and repositories: the model-call log, a job's memo of its calls (ADR-0020), the battle log of its missions (ADR-0011); jobs, and the stage store: artifacts kept once by hash, the stage cache, each job's runs of its stages, committed together (ADR-0004) | core, db, json, llm, agents |
| `rr_runtime_client` *(M0)* | The hub's side of the pod: the runtime protocol, readiness, the runtime's model provider, `PodManager` | runtime_api, runpod, net, llm |
| `rr_hub` *(M0)* | The hub's configuration; `ModelStack`, the providers every model call goes through (the runtime only, ADR-0017); a job's army, raised the first time a stage needs a model (`JobArmy`: the pod, its engines, the job's memo, the battle log); operator tools: `doctor`, `bench` ([design](design/hub.md)) | config, runtime_client, library, llm, agents |
| `rr_text` *(M1)* | Text made comparable: UTF-8 checked and repaired, NFC to keep and a fold to compare; tokens and terms; sentences, with nothing lost; numbers by value, spoken ones too; names and mentions; quotes aligned with what was said (Smith–Waterman); BM25 retrieval. Fuzzed ([design](design/text.md)) | core, utf8proc |
| `rr_media` *(M1)* | Where a video comes from: yt-dlp's description of it, the captions chosen in the language spoken (never a translation), yt-dlp run safely with its failures told in rReport's terms ([design](design/transcript.md)) | core, json, proc, transcript |
| `rr_transcript` *(M1)* | Captions into the transcript: json3, WebVTT, SubRip and plain text read into timed words (markup, sounds and speakers set apart); a model's punctuation laid onto automatic captions without changing a word; the words cut into numbered, timed sentences ([design](design/transcript.md)) | core, knowledge, json, text |
| `rr_fidelity` *(M1)* | The deterministic checks: Ledger A (every sentence accounted for, once; atoms' numbers and must-quotes said; no filler hiding a number, a name or a term) and Ledger B (markers valid, every atom told, numbers, names and terms where cited, quotations verbatim, the talk's order), each finding with its repair message; the coverage that M1's acceptance measures ([design](design/fidelity.md)) | core, knowledge, text |
| `rr_pipeline` *(M1)* | A job's stages, each run or found done in the stage cache and committed with its artifact checked against its schema; cache keys over the stage, its schema, its fingerprint (prompts, models, settings) and its inputs' artifacts; `run_job()`, a job taken through them and saved. The first stages: `source` (a video through yt-dlp, or a caption file, read once) and `transcript` (captions into numbered sentences, punctuated by the infantry where they are not) ([design](design/pipeline.md)) | core, json, db, library, agents, prompt, media, transcript, knowledge, llm |
| `rr_eval` *(M2)* | The evaluation's data: the set of videos, strictly read; snapshots of their sources, read once and pinned by hash; gold annotations anchored to the timeline; silver drafted by the `annotate` stage (the commander's three drafts per window, joined, then the other family's additions), windows fitted to the engines; the review sheet ([design](design/eval.md)) | core, json, library, pipeline, agents, prompt, knowledge, media, text, transcript, toml++ |
| `rr_report` (M1) | Rendering the article | all of the above |
| `rr_knowledge` *(M1)* | The knowledge model as the stages write it: the short IDs `S#`, `A#`, `V#`; the transcript; Ledger A, the atoms and fillers every sentence became (ADR-0005); the article and its `[A#]` markers, read by a fuzzed parser (ADR-0006); each artifact strictly checked against its compiled-in schema (ADR-0004) ([design](design/knowledge.md)). Repositories and projections arrive with the pipeline | core, json, text |
| `rr_research`, `rr_hub_api` (M8–M9) | Research tools, REST/SSE for the UI and MCP | … |
| `rr_speech` (M4) | Parakeet, diarization, Whisper: **the only CUDA code** | sherpa-onnx, whisper.cpp |

## 4. Cross-cutting rules

- **Errors are values.** `rr::Result<T>` everywhere a failure is expected; exceptions never cross a module boundary (ADR-0007).
- **Cancellation is cooperative.** Every blocking operation accepts a `std::stop_token` (ADR-0010).
- **No hidden globals.** State is passed explicitly (`LibraryContext`, clients, clocks). The logger is the one exception.
- **Determinism where it matters.** Prompt building is byte-deterministic (canonical JSON, no timestamps in prefixes), so caching works and tests can record and replay model calls.
- **Secrets never print.** `rr::Secret` has no implicit conversion to a string, and formats as `***`.

## 5. Data flow of one video (target shape, M1 onward)

`resolve → capture → frames → lexicon-0 → speech → visuals → transcript → sections + brief → atomize → structure → compose → verify → enrich → index`

Each arrow is a stage that writes one immutable artifact (ADR-0004). Map-shaped stages run as infantry squads; global passes and writing run on the commander (ADR-0011). Stage details: plan §5.

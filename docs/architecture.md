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
| `rr_core` *(M0)* | `Result`/`Error`, strong IDs + ULIDs, time spans + clocks, BLAKE3/SHA-256, `Secret`, `RR_ASSERT`, `ThreadPool`, `Limiter`, careful POSIX file I/O (EINTR, short writes, fsync of files and directories, whole files replaced atomically) | std, BLAKE3, OpenSSL (crypto) |
| `rr_log` *(M0)* | Structured logging (console + rotating JSONL) with per-thread context | core, nlohmann_json |
| `rr_config` *(M0)* | Layered TOML config with value origins and secrets | core, toml++ |
| `rr_json` *(M0)* | JSON parse/dump, canonical form, JSON-Schema validation, portable-schema lint; schemas keep their field order for models | core, nlohmann_json, jsoncons |
| `rr_db` *(M0)* | SQLite wrapper (WAL, writer + reader pool), migrations ([design](design/database.md)) | core, SQLite |
| `rr_blob` *(M0)* | Content-addressed, crash-safe blob store (BLAKE3 names, atomic publish) | core |
| `rr_proc` *(M0)* | Subprocesses: run-to-completion and supervised children, each in its own process group (ADR-0013); termination signals as a stop request | core (posix_spawn) |
| `rr_net` *(M0)* | HTTP client (libcurl), SSE parser, retry policy, HTTP server (cpp-httplib) ([design](design/networking.md)) | core, json, libcurl, cpp-httplib |
| `rr_runtime_api` *(M0)* | Hub↔runtime protocol v1: paths, the health report, the streamed-call contract | core, json |
| `rr_runpod` *(M0)* | RunPod's pod API (REST v2): read, start and stop a pod; a pod stopping itself (ADR-0015) | core, net, json |
| `rr_supervisor` *(M0)* | Runtime: auth, health, engines, the streaming relay (ADR-0014), model files (verified, resumable downloads), blobs, idle/dead-man stop through RunPod's API (ADR-0015), the runtime's configuration ([design](design/runtime.md)) | runtime_api, net, proc, blob, config, runpod, json, log, toml++ |
| `rr_llm` *(M0)* | Calling models: one chat vocabulary, the OpenAI-compatible streaming provider, retries, accounting, record/replay, schema-validated answers ([design](design/llm.md)) | core, json, net, log |
| `rr_library` *(M0)* | A library's database (ADR-0008): its schema (`migrations/`, compiled in) and repositories, starting with the model-call log | core, db, json, llm |
| `rr_runtime_client` *(M0)* | The hub's side of the pod: the runtime protocol, readiness, the runtime's model provider, `PodManager` | runtime_api, runpod, net, llm |
| `rr_hub` *(M0)* | The hub's configuration and operator tools: `doctor`, `bench` ([design](design/hub.md)) | config, runtime_client, library, llm |
| `rr_agents` (M1) | Missions, squads, dispatcher, escalation, battle log | llm, db |
| `rr_media`, `rr_text`, `rr_transcript` (M1) | Capture sources, captions, frames, alignment | proc, blob |
| `rr_pipeline`, `rr_fidelity`, `rr_report` (M1) | Stages + executor, ledger checks, rendering | all of the above |
| `rr_knowledge` (M1+) | VKM types, repositories, projections | db, json |
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

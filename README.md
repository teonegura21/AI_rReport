# rReport

**Understand any video completely, then go beyond it.**

rReport turns a YouTube URL into a **Retold Article**: a readable, long-form reconstruction of *everything* the video says and shows. Nothing is dropped:
- every argument, example, number, name and aside;
- on-screen code, slides and charts;
- blunt statements, quoted verbatim.

A measured **Fidelity Report** proves the coverage. rReport then helps you **complete the video's story**:
- an **Idea Map** you can question branch by branch;
- automatic context (glossary, entity cards, assumed knowledge, claims to check);
- resumable **research threads** that go further than the video did.

It runs on open models you control:
- **Qwen3.8 as the commander**, which writes, reasons and converses;
- **a swarm of quantized Gemma 4 "infantry"**, which runs small, verifiable missions in parallel;

all on a rented GPU pod, orchestrated by a C++ hub on your own machine.

> **Status: M0 (foundations) in progress.** See [the plan](docs/plan.md) for the full roadmap (M0–M10, then the browser extension and the hosted SaaS).

## Why it's different

| Typical video AI | rReport |
|---|---|
| Summarizes (~5% of the transcript) | Retells everything (typically 40–80% of the transcript) |
| Reads captions only | Transcript **and** what's on screen |
| Garbles jargon and names | Lexicon-first transcription, three-way voting, evidence-based correction |
| Softens blunt content | `must_quote` statements verified verbatim by code |
| Trust us | Coverage measured by the Fidelity Ledger, with gaps shown, not hidden |
| Stops where the video stops | Fact-checks, "what changed since", research threads |

## Architecture in one picture

```
Laptop (CPU): rreport hub — capture · pipeline · Fidelity Ledger · agents · library · web UI · MCP
                    │  HTTPS + token (blobs, jobs, streamed model calls)
GPU pod:      rreport-runtime — CUDA-free supervisor → llama-server (commander, infantry, embedder)
                                                     → rr-speech-worker (Parakeet, Whisper, diarization)
```

Details: [architecture](docs/architecture.md) · [decisions (ADRs)](docs/adr/README.md) · [glossary](docs/glossary.md).

## Building

Requirements:
- Linux (macOS should work);
- **GCC ≥ 13 or Clang ≥ 19**, CMake ≥ 3.28, Ninja;
- system packages for libcurl, OpenSSL and SQLite.

Libraries missing from the system are fetched at pinned versions automatically (ADR-0012).

```bash
# Ubuntu 24.04: the fastest path installs most dependencies from apt
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
  libcurl4-openssl-dev libssl-dev libsqlite3-dev nlohmann-json3-dev libspdlog-dev \
  libfmt-dev libgtest-dev libgmock-dev libcli11-dev libtomlplusplus-dev

cmake --preset dev             # GCC, debug, warnings as errors
cmake --build --preset dev
ctest --preset dev             # all unit tests

cmake --preset dev-clang       # Clang >= 19 with AddressSanitizer + UBSan
cmake --build --preset dev-clang && ctest --preset dev-clang
```

Other presets: `tsan` (ThreadSanitizer), `release`, `runtime-release` (the supervisor only).

## Repository map

```
src/<module>/        C++ modules (public headers in src/<module>/include/rr/<module>/)
apps/                executables: rreport (hub + CLI), rreport-runtime (pod supervisor)
tests/unit/          GoogleTest suites, one per module
cmake/               build helpers: warnings, sanitizers, module layering, dependencies, embedding
config/ migrations/  default configuration and database migrations (compiled into binaries)
docker/ deploy/      runtime image and RunPod runbook
docs/                plan, architecture, ADRs, design notes, runbooks
```

## Conventions

[Coding conventions](docs/design/coding-conventions.md):
- `std::expected` errors;
- stop tokens everywhere;
- *why*-comments;
- one ADR per decision.

## License

Private and proprietary. rReport is developed as a hosted SaaS (see plan §14).

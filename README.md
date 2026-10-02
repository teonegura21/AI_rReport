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

> **Status: M0 (foundations) done; M1 and M2, the intelligence core, in progress** ([phase by phase](docs/roadmap.md)). See [the plan](docs/plan.md) for the full roadmap (M0–M10, then the browser extension and the hosted SaaS).

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

Details: [architecture](docs/architecture.md) · [decisions (ADRs)](docs/adr/README.md) · [glossary](docs/glossary.md) · [roadmap, phase by phase](docs/roadmap.md).

## Building

Requirements:
- Linux (macOS should work);
- **GCC ≥ 13 or Clang ≥ 19**, CMake ≥ 3.28, Ninja;
- system packages for libcurl, OpenSSL and SQLite.

Libraries missing from the system are fetched at pinned versions automatically (ADR-0012).

```bash
# Ubuntu 24.04: the fastest path installs most dependencies from apt
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
  libcurl4-openssl-dev libssl-dev libsqlite3-dev nlohmann-json3-dev \
  libgtest-dev libgmock-dev libcli11-dev libtomlplusplus-dev

cmake --preset dev             # GCC, debug, warnings as errors
cmake --build --preset dev
ctest --preset dev             # all unit tests

cmake --preset dev-clang       # Clang >= 19 with AddressSanitizer + UBSan
cmake --build --preset dev-clang && ctest --preset dev-clang
```

Other presets: `tsan` (ThreadSanitizer), `release`, `runtime-release` (the supervisor only).

**Before you push**, run what CI runs (`.github/workflows/ci.yml`). The formatting check covers every file, not only the ones you touched:

```bash
git ls-files '*.hpp' '*.cpp' | xargs clang-format-20 --dry-run --Werror
cmake --preset tidy && cmake --build --preset tidy     # clang-tidy, warnings as errors
for preset in dev dev-clang tsan; do     # stops at the first failure
    cmake --preset $preset && cmake --build --preset $preset && ctest --preset $preset || break
done
shellcheck --severity=style docker/runtime/entrypoint.sh    # apt-get install shellcheck
python3 -m unittest discover -s docker/runtime -p 'test_*.py'   # the pod image's entry point
python3 -m unittest discover -s scripts -p 'test_*.py'
```

A nightly job (`.github/workflows/nightly.yml`) also runs the runtime against a real llama-server, built for the CPU, with a tiny model made on the spot: [docs/design/runtime.md](docs/design/runtime.md#the-real-engine-nightly-l4) says what it checks, and how to run it yourself.

Every rule a module keeps is mutation-tested: removed on purpose, and a test must fail (`scripts/mutate.py tests/mutants/<module>.toml`). Code that reads untrusted input is fuzzed: every push replays the committed corpora, and the `fuzz` job searches for new inputs with libFuzzer (`cmake --preset fuzz`). [docs/design/testing.md](docs/design/testing.md) describes the test layers, the stand-ins (the fake engine, the oracle, the fault injectors) and the practice.

## Running the pod

The GPU pod runs `rreport-runtime` from a CUDA image (`docker/runtime/`, built by `.github/workflows/runtime-image.yml`). **[deploy/runpod/README.md](deploy/runpod/README.md)** takes you from a green CI to a measured pod:
1. check the model checksums;
2. build the image;
3. set up the volume, the pod and the hub;
4. run the M0 checks that only a real pod can settle;
5. run the benchmark that chooses the default GPU.

Then **[docs/runbooks/m1-m2.md](docs/runbooks/m1-m2.md)** lists what only the pod can measure for the intelligence core, step by step as the phases land. R1 to R3 are ready now: the commander's parallel slots and the prompt cache; the eval's videos and their gold annotations; and M1's acceptance, three videos ingested at the `max` tier and read.

## Using it

So far a job reads its source, makes its transcript, outlines the video, writes Ledger A, what became of every sentence, plans the article, which paragraph tells which atoms, and writes it, every sentence citing the atoms it tells ([docs/design/pipeline.md](docs/design/pipeline.md)). A judge of another family than the writer reads the best drafts, sentence by sentence, and picks one. Where two sections meet, the commander rewrites the two paragraphs together so the article reads on, and the rewrite is kept only where the judge reads it no less faithful. Each job's article is one web page, written by `rreport report`: every sentence starts the video where it was said, and the Fidelity Report under it says what was kept, against M1's targets, and what was left out, and why ([docs/design/report.md](docs/design/report.md)). What needs no model runs without a pod: the source and the transcript of a video with its creator's captions, or of a caption file. The atoms need the pod: without one, a job stops there and says so, and `rreport resume` takes it up once a pod is set up.

```bash
rreport ingest 'https://www.youtube.com/watch?v=…'   # or a caption file: .vtt, .srt, .json3, .txt
rreport jobs                                          # the library's jobs, newest first
rreport jobs show <job>                               # each stage, and what it could not do
rreport resume <job>                                  # a stopped job, taken up where it stopped
rreport report <job>                                  # its article as a web page, with the Fidelity Report
```

Automatic captions come without punctuation: the pod's infantry punctuates them, without changing a word. Without a pod, they are cut where the speaker paused, and the job says so.

**Measuring it** ([docs/design/eval.md](docs/design/eval.md)): the eval's videos, read once and kept, are taken through the stages a number of times each, and every article is scored by code against what a person wrote down the video says, and read by frozen judges: two families of models, never the pipeline's own judges, asked the same way every time.

```bash
rreport eval run --label baseline      # the dev videos, three times each, at max; again: taken up
rreport eval score --label baseline    # each article scored: what it kept, against the gold, and its cost
rreport eval judge --label baseline    # read by the frozen judges: facts told, sentences the video supports
rreport eval export --label baseline   # a row per video and replicate (CSV; --format json)
rreport eval compare baseline candidate   # every metric, video by video, with its 95% interval
rreport eval gate baseline candidate      # may the candidate replace the baseline? (eval/gates/default.toml)
```

## Repository map

```
src/<module>/        C++ modules (public headers in src/<module>/include/rr/<module>/)
apps/                executables: rreport (hub + CLI), rreport-runtime (pod supervisor)
tests/unit/          GoogleTest suites, one per module
cmake/               build helpers: warnings, sanitizers, module layering, dependencies, embedding
config/ migrations/  default configuration and database migrations (compiled into binaries)
prompts/ schemas/    the prompts, and the JSON schemas of answers and artifacts (compiled in)
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

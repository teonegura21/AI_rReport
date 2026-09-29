# ADR-0003: GPU engines run as child processes of a CUDA-free supervisor

- Status: Accepted
- Date: 2026-09-29

## Context

The runtime must host several GPU engines:
- llama.cpp's `llama-server` for the commander, the infantry and the embedder;
- whisper.cpp for Whisper;
- sherpa-onnx (ONNX Runtime) for Parakeet and diarization.

Three facts make putting them in one process fragile:

1. **Memory release is unreliable.** ONNX Runtime's CUDA arena and ggml's CUDA memory pool do not reliably return VRAM, so an in-process "unload" may not free memory.
2. **Crashes propagate.** A CUDA fault or an assert inside any engine would kill the whole API server.
3. **Duplicate ggml.** whisper.cpp and llama.cpp each vendor their own copy of ggml, and linking both into one binary invites symbol and version clashes.

## Decision

- **`rreport-runtime` is a supervisor that never links CUDA.** It owns:
  - authentication, blobs and jobs;
  - the OpenAI-compatible proxy;
  - the VRAM policy (from a GPU *profile*);
  - health, metrics, and idle/dead-man shutdown.
- **Every GPU engine is a child process** bound to `127.0.0.1`:
  - one upstream `llama-server` per resident model;
  - `rr-speech-worker`, a one-shot batch CLI (manifest in, JSON out, exit), from M4.
- **Unloading a model means the process exits**, which is the only way to *guarantee* its VRAM is released.
- The supervisor restarts a crashed engine with exponential backoff and reports it in `/health`.

## Consequences

- Upstream binaries run unmodified, so upgrading llama.cpp is a tag bump plus an integration test.
- The supervisor builds and runs on machines with no GPU, which makes it easy to test in CI with fake engines.
- A crashed engine is isolated. In-flight requests to it fail with `unavailable` and are retried by the hub.
- Cost: starting a process and loading weights takes seconds. The pipeline therefore batches work by phase (speech → infantry → commander), so each swap is paid once per phase, not once per call.

## Alternatives considered

- **Link libllama and whisper.cpp in-process.** Lower latency on model swaps, but none of the isolation or memory guarantees. Rejected.
- **llama-server "router mode" as the only scheduler.** Useful for A/B evaluation, but VRAM policy must also cover the speech engines, so the supervisor owns scheduling.

# ADR-0004: Stage artifacts are the source of truth

- Status: Accepted
- Date: 2026-09-29

## Context

A video goes through ~14 stages, many of which call LLMs. We need:
- **Crash consistency.** A `kill -9` at any point must not leave half-written state.
- **Resumability.** Finished work is never paid for twice.
- **Cheap experiments.** Changing one prompt must not re-run the whole pipeline.
- **Reproducibility.** We must know exactly which models and prompts produced a report.

## Decision

- **One artifact per stage.** Each stage emits one immutable, schema-validated JSON document, stored zstd-compressed in the `artifacts` table and addressed by its content hash.
- **Cache key.** A stage's key is `BLAKE3(stage@version, prompt and schema content hashes, model id + quantization, params, hashes of the input artifacts)`. `stage_cache` maps key → artifact.
- **Projections.** Relational tables (`sentences`, `atoms`, `ideas`, …) are *projections* rebuilt from artifacts. They exist for querying, not as the record.
- **Atomic commit.** The artifact, the stage status and the projections commit in **one SQLite transaction**. That transaction is the crash-consistency point.
- **Analyses.** An **analysis** is the configuration snapshot of one run. A video can have several; the UI shows the current one.

## Consequences

- Resume = look up each stage's cache key. A hit means "done".
- A new composition prompt creates a new analysis in which every upstream stage is a cache hit. Only composition and verification re-run.
- Artifacts double as golden files for tests and as a reproducibility record.
- Cost: some duplication between artifacts and projections. Acceptable, because projections are derived data.

## Alternatives considered

- **Tables as the only record, mutated in place.** Hard to make crash-safe across LLM calls, and impossible to diff runs. Rejected.
- **Files on disk per stage.** Simple, but loses transactional coupling with the database. Rejected.

# ADR-0006: Citation markers instead of prose inside JSON

- Status: Accepted
- Date: 2026-09-29

## Context

Every article sentence must be traceable to its atoms, so we can check coverage and seek the video player.
- Asking a model to emit long prose *inside* JSON strings degrades writing quality: escaping, and models writing "for the parser".
- Models also hallucinate timestamps and IDs.

## Decision

- **Markers.** The composer writes **Markdown with inline markers**, e.g. `…data curation [A41].`. Markers are parsed deterministically.
- **No computed values from models.** Models never emit timestamps, or any ID or number the code can compute. Timestamps are rendered from atom spans.
- **Structured outputs** (JSON schema) are used only for extraction-style missions, where the output is naturally a record.
- **Own citations.** Citations in chat and research use the same idea (`[S123]`, `[src:4]`), verified by code. We don't depend on a provider-specific citation feature (Claude's citations cannot be combined with structured outputs anyway).

## Consequences

- Better prose, deterministic parsing, and one citation mechanism across all providers.
- The marker parser is security- and correctness-critical. It gets unit tests and fuzzing.

## Alternatives considered

- **JSON `paragraphs → sentences → atom_ids`.** Precise, but it hurts prose quality and bloats tokens. Rejected.

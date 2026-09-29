# ADR-0009: Cross-analysis data is anchored to the video timeline

- Status: Accepted
- Date: 2026-09-29

## Context

A video can be re-analyzed: better models, new prompts. Atom and sentence IDs are *per analysis*.

Research threads, conversations and claim verdicts are long-lived user data. Linking them to atom IDs would orphan them the moment a new analysis replaces the old one.

## Decision

- **Inside an analysis**, data uses short, model-friendly IDs (`S#`, `A#`, `V#`, `I#`) plus time spans `(t0_ms, t1_ms)`.
- **Outside an analysis** (research, chats, verdicts), data carries an **anchor**: `{video_id, t0_ms, t1_ms, quote, analysis_id?, ref?}`. When the current analysis changes, anchors are re-resolved by time overlap and quote match.
- **Claim verdicts are append-only**, each with an "as of" date. That history powers "what changed since".

## Consequences

- User research survives any number of re-analyses.
- Re-resolution is fuzzy when a new transcript differs. The quote match plus time overlap makes it robust in practice, and unresolved anchors are shown, never dropped.

## Alternatives considered

- **Foreign keys to atom IDs.** Brittle across analyses. Rejected.

# Architecture Decision Records

An ADR records one significant decision: the **context** that forced it, the
**decision** itself, its **consequences** (good and bad), and the
**alternatives** we rejected. ADRs are immutable once accepted. To change a
decision, write a new ADR that supersedes the old one and link them both ways.

| #    | Decision | Status |
|------|----------|--------|
| 0001 | [Hub/runtime split](0001-hub-runtime-split.md) | Accepted |
| 0002 | [C++ core, TypeScript at the edges](0002-cpp-core-ts-edges.md) | Accepted |
| 0003 | [GPU engines run as child processes of a CUDA-free supervisor](0003-gpu-engines-as-processes.md) | Accepted |
| 0004 | [Stage artifacts are the source of truth](0004-artifacts-are-truth.md) | Accepted |
| 0005 | [Closed-world atomization (the Fidelity Ledger)](0005-closed-world-atomization.md) | Accepted |
| 0006 | [Citation markers instead of prose inside JSON](0006-citation-markers.md) | Accepted |
| 0007 | [Errors are values: `std::expected`](0007-errors-std-expected.md) | Accepted |
| 0008 | [One SQLite database per library](0008-sqlite-per-library.md) | Accepted |
| 0009 | [Cross-analysis data is anchored to the video timeline](0009-timeline-anchors.md) | Accepted |
| 0010 | [Threads and stop tokens, not coroutines](0010-threads-not-coroutines.md) | Accepted |
| 0011 | [Commander and infantry: a native sub-agent environment](0011-commander-infantry.md) | Accepted |
| 0012 | [Dependencies: system packages first, pinned git fallback](0012-dependency-acquisition.md) | Accepted |
| 0013 | [Subprocesses run through posix_spawn, each in its own process group](0013-subprocesses-posix-spawn.md) | Accepted |
| 0014 | [The pod's relay keeps streams alive and never disguises a failure](0014-honest-streaming-relay.md) | Accepted |

Template for new ADRs:

```markdown
# ADR-NNNN: Title

- Status: Proposed | Accepted | Superseded by ADR-XXXX
- Date: YYYY-MM-DD

## Context
## Decision
## Consequences
## Alternatives considered
```

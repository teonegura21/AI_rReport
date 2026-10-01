# ADR-0007: Errors are values: `std::expected`

- Status: Accepted; in part superseded by [ADR-0017](0017-local-models-only.md): `refused` goes to the other local model family, not to a fallback provider
- Date: 2026-09-29

## Context

Most failures in this system are *expected*: timeouts, rate limits, invalid model output, missing files, a pod that disappeared.
- They must be handled explicitly, retried by class, and reported with context.
- Exceptions make these paths invisible in function signatures.
- Many of our dependencies are C libraries (SQLite, libcurl) that report errors as codes anyway.

## Decision

- **Signatures.** Every function that can fail in an expected way returns `rr::Result<T> = std::expected<T, rr::Error>`.
- **`rr::Error`** carries:
  - an error class (`rr::Errc`);
  - a message;
  - a context chain (added while unwinding);
  - an optional `retry_after`;
  - the `std::source_location` where it was created.
- **Propagation.** `RR_TRY(expr)` and `RR_TRY_ASSIGN(decl, expr)` propagate errors without GNU statement expressions, so the code stays portable to MSVC.
- **Exceptions** are allowed only *inside adapters* that wrap throwing libraries (jsoncons, inja, cpp-httplib, toml++). The adapter converts them to `Error` at its boundary. A catch-all at every thread entry turns an escaped exception into `Errc::internal`, so a bug fails one task, not the process.
- **Programmer errors** (violated invariants) use `RR_ASSERT`, which is always on and aborts with the source location.
- **Retry policy by class:**
  - transient (`network`, `timeout`, `unavailable`, `rate_limited`) → backoff with jitter;
  - `model_output_invalid` → one repair, then a fallback model;
  - `refused` → the fallback provider;
  - `budget_exceeded` / `needs_user` → stop and surface.

## Consequences

- Failure paths are visible in types and enforced with `[[nodiscard]]`.
- Requires GCC ≥ 13 or Clang ≥ 19. Clang 18's concepts feature level hides `std::expected` in libstdc++, and CMake enforces this with an explanatory error.

## Alternatives considered

- **Exceptions everywhere.** Invisible control flow, and awkward across C callbacks and threads. Rejected.
- **`tl::expected` or a custom type.** Unnecessary now that the standard type is available.

# Coding conventions

These rules exist so that any file in the repository reads like every other file. Tooling enforces most of them (`.clang-format`, `.clang-tidy`, compiler warnings). The rest are enforced in review.

## Language and toolchain

- **C++23.** GCC ≥ 13 or Clang ≥ 19 (Clang 18 cannot see `std::expected` in libstdc++; see ADR-0007). CMake ≥ 3.28, Ninja.
- **Warnings** are errors in CI. Sanitizer builds: ASan + UBSan (`dev-clang`), TSan (`tsan`).
- **No compiler extensions.** No GNU statement expressions and no VLAs, so an MSVC port stays possible.

## Naming

| Entity | Style | Example |
|---|---|---|
| Namespaces | `snake_case` | `rr::db` |
| Types, aliases, concepts, template params | `CamelCase` | `BlobStore`, `Result<T>` |
| Functions, methods, variables, parameters | `snake_case` | `put_file()`, `retry_after` |
| Private/protected data members | `snake_case_` | `root_` |
| Constants (`constexpr`, globals) | `kCamelCase` | `kDefaultTimeout` |
| Type-trait variable templates | `snake_case_v`, like the standard library | `is_result_v<T>` |
| Enumerators | `snake_case` (like `std::errc`) | `Errc::not_found` |
| Macros | `RR_UPPER_CASE` | `RR_TRY` |
| Files | `snake_case.hpp/.cpp` | `thread_pool.hpp` |

## Errors (ADR-0007)

- Fallible functions return `rr::Result<T>` and are `[[nodiscard]]`.
- Create errors with `rr::fail(Errc, message)`. Add context while unwinding with `.with_context("while …")`.
- Propagate with `RR_TRY(expr);` or `RR_TRY_ASSIGN(auto x, expr);`.
- Exceptions: only inside adapter `.cpp` files that wrap throwing libraries, converted at the boundary.
- Invariants: `RR_ASSERT(cond, "message")` is always on. Use it for *programmer* errors, never for input validation.

## Ownership and lifetimes

- **Value semantics by default.** Move-only RAII wrappers own resources (sqlite3*, CURL*, file descriptors, processes).
- **No raw `new`/`delete`.** Use `std::unique_ptr` for polymorphism or pimpl, and `std::shared_ptr` only for genuinely shared ownership.
- **Parameters:** views (`std::string_view`, `std::span`) for read-only input; values for sinks that are moved in.
- **Third-party types stay out of public headers.** Hide them behind pimpl or narrow value types.

## Concurrency (ADR-0010)

- Every blocking function takes a `std::stop_token` and honors it promptly.
- Shared mutable state is guarded by a mutex *declared next to the data it protects*, with a comment naming the invariant.
- Document thread-safety at the top of each class: "thread-safe", "externally synchronized", or "confined to one thread".

## Comments and documentation

- Every public header opens with a block that explains **why the module exists**, its **invariants** and its **thread-safety**.
- Comments explain *why*, not *what*. If code needs a *what* comment, rename or restructure it.
- Decisions that affect more than one module get an ADR in `docs/adr/`.

## Tests

- GoogleTest. One test executable per module: `tests/unit/<module>/`.
- **Name** tests `Subject_Scenario_Expectation`, e.g. `Limiter_CancelledWaiter_ReturnsCancelled`.
- **Tests must be hermetic.** Temp directories, fake clocks, in-process HTTP servers, no network, no sleeps longer than a few milliseconds.
- **Every bug fix adds the test that would have caught it.**

## Commits

- Imperative subject ≤ 72 characters ("Add BLAKE3 content hashing"). The body explains *why*.
- One logical change per commit. Formatting-only changes go in their own commit.

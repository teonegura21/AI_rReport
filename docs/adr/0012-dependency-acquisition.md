# ADR-0012: Dependencies: system packages first, pinned git fallback

- Status: Accepted (refines the plan's "vcpkg manifest mode" line)
- Date: 2026-09-29

## Context

The plan proposed vcpkg in manifest mode. When implementation started, the development container turned out to be restrictive:
- **GitHub source tarballs** (codeload.github.com), which vcpkg ports download by default, are **blocked**.
- `git clone` from GitHub works.
- Ubuntu's package archive works.

The owner's laptop is Linux (apt available). CI runners and Docker builds have full network access.

## Decision

Every third-party library is declared once in `cmake/RrDependencies.cmake` using CMake ≥ 3.24's `FetchContent_Declare(... FIND_PACKAGE_ARGS ...)`:

1. **`find_package` first**, with a minimum version. This picks up system packages (apt), a vcpkg toolchain or Conan — whatever the environment provides.
2. **If not found**, CMake clones the **pinned git tag** and builds it as part of the tree.
3. **Large C libraries** (libcurl, OpenSSL, SQLite) are **system-only** (`REQUIRED`). Building them from source would dominate build time and add risk for no benefit.

## Consequences

- One mechanism works in a restricted container, on a developer laptop, in CI and in Docker. There is no extra tool to bootstrap.
- Versions are pinned in a single file, with minimum versions enforced for system packages.
- vcpkg or Conan can still drive the build: pass their toolchain file and `find_package` resolves to their packages.
- System packages mean the exact version may differ across distros. Minimum versions plus CI on the reference distro (Ubuntu 24.04) keep this in check.

## Alternatives considered

- **vcpkg manifest mode.** Blocked in the development environment, heavier CI setup. Still usable through its toolchain, see above.
- **Conan 2.** Another tool and package center to depend on.
- **Vendoring sources / git submodules.** Repository bloat and update friction.

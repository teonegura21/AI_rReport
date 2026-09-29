# ADR-0013: Subprocesses run through posix_spawn, each in its own process group

- Status: Accepted (replaces the plan's choice of reproc for `rr_proc`)
- Date: 2026-09-29

## Context

rReport runs external programs everywhere:
- **On the hub:** yt-dlp, ffmpeg and ffprobe.
- **On the pod:** one llama-server per model, whisper-cli, rr-speech-worker and nvidia-smi (ADR-0003).

The plan named reproc as the subprocess library. While implementing `rr_proc` we read its POSIX source and found three gaps that matter for these workloads:

1. **No process groups on POSIX.**
   - reproc signals only the direct child. It puts children in their own group only on Windows.
   - yt-dlp spawns ffmpeg. Stopping yt-dlp on a timeout or cancel leaves ffmpeg running as an orphan.
   - The orphan inherited our output pipes, so the reader waits for an EOF that arrives only when the orphan finishes on its own. That can be many minutes, and the orphan burns CPU all the while.
2. **Inherited signal dispositions.**
   - A server ignores SIGPIPE, so a failed socket write returns an error instead of killing the process.
   - A child inherits SIG_IGN across exec, so pipelines such as `yes | head` misbehave in children.
3. **Environment overrides that do not override.**
   - reproc's "extend" mode appends variables to the parent's environment.
   - An override like `CUDA_VISIBLE_DEVICES=1` then comes *after* the inherited value, and getenv() returns the first match. The override is silently ignored.

A fourth trap: with default options, reproc's destructor waits for the child *indefinitely*. It can be configured away, but only by remembering to.

## Decision

`rr_proc` starts processes with **`posix_spawn`** and manages them itself (`src/proc/process.cpp`).

**Start state:**
- **Process group:** each child leads its own group (`POSIX_SPAWN_SETPGROUP`).
- **Signals:** all signal dispositions are reset to default (`POSIX_SPAWN_SETSIGDEF`), and nothing is blocked (`POSIX_SPAWN_SETSIGMASK`).
- **stdin:** `/dev/null`, so a child never reads our terminal (ffmpeg would).
- **Environment:** inherited or hermetic. Overrides *replace* same-named variables.
- **Executable:** resolved before spawning, so a missing tool is a clear `Errc::not_found`.

**Stopping and reaping:**
- **Escalation:** SIGTERM to the whole group, a grace period, then SIGKILL to the group.
- **No leftovers:** when the leader exits, whatever remains in its group is killed.
- **No PID reuse:** exit is detected with `waitid(..., WNOWAIT)`, so the group is signalled only while the unreaped (zombie) leader still reserves its ID. A signal can never reach an unrelated process that reused the ID.

**One thread per child:**
- A single "pump" thread per child polls its pipes, applies stop requests and reaps it. Signalling and reaping never race, and output is always drained, so a chatty child never blocks.
- A quiet child costs ten wake-ups a second (a 100 ms slice); a stop is noticed within that slice.

## Consequences

- Stopping a tool stops everything it started. The pod's engines, and the hub's capture tools, cannot leak processes or pin pipes.
- One dependency fewer.
- POSIX only (Linux and macOS). The hub targets Linux and the runtime runs in Linux containers. Windows would need a separate implementation behind the same interface (job objects are the Windows analogue of process groups).
- `posix_spawn_file_actions_addchdir_np` is a (widely available) extension: glibc ≥ 2.29, macOS ≥ 10.15.
- rReport must never set SIGCHLD to SIG_IGN, or the kernel would reap children behind the pump's back. This is documented in the header.

## Alternatives considered

- **Keep reproc and wrap its gaps.** Process groups would need its `fork` mode plus our own exec and environment handling. That means reimplementing most of what we use it for.
- **fork + exec by hand.** Unsafe in a multi-threaded program unless the child sticks to async-signal-safe calls. `posix_spawn` encodes exactly that safe subset, and glibc implements it with `CLONE_VFORK`, which also reports exec failures synchronously.
- **Boost.Process.** A large dependency, and its v1/v2 API churn was a concern. The same process-group work would be needed.
- **pidfd plus a wake-up pipe** for an event-driven pump with no periodic wake-ups. Rejected for now: more moving parts for no benefit we need. Revisit if a pod ever supervises hundreds of children.

# ADR-0016: A pod that cannot start stops itself

- Status: Accepted
- Date: 2026-09-30

## Context

The pod image's entry point (`docker/runtime/entrypoint.sh`) prepares the pod, then runs the runtime. Several things can stop it from ever serving:
- no GPU is visible (a driver or host problem);
- the hub's token is not set;
- a model file cannot be downloaded or does not match its checksum (a wrong catalog entry, Hugging Face down, a full volume);
- the runtime refuses its configuration or dies.

RunPod restarts a container whose command exits (ADR-0015, "Exiting is not stopping"). An entry point that simply exits on such a failure therefore restarts, fails the same way, and restarts again. The pod bills by the minute, and nothing in that loop ever stops it. A failed model sync is the costliest case: each restart may download gigabytes again before failing.

## Decision

On any failure before or during serving, the entry point stops the pod through RunPod's API, then waits to be taken down:

1. **Stop.** It runs `rreport-runtime stop-pod`, the runtime's own self-stop (ADR-0015). That command needs only RunPod's variables and the configuration's `idle` section: no GPU, no token, no catalog, since any of those may be what is missing.
2. **Wait.** It sleeps until RunPod's stop signal arrives, then exits 0. It never exits on its own on RunPod, because an exit is a restart.
3. **Say why.** The reason is the last thing on the pod's log, where the operator looks first.
4. **Outside RunPod** (no `RUNPOD_POD_ID`), nothing restarts the container in a loop, so the entry point simply exits 1.

To see how the runtime ended, bash stays PID 1 instead of `exec`ing it. Each step runs as a child, and a stop signal is passed on to it. The kernel gives PID 1 no default signal handling, so without that forwarding, `docker stop` would wait out its grace period and kill everything.

## Consequences

- A broken pod costs minutes, not a night. The hub sees the pod `EXITED` instead of `RUNNING`, and `PodManager` reports it ("is EXITED instead of starting") once the start's grace period has passed.
- **The script is tested as a program** (`docker/runtime/test_entrypoint.py`). The real script runs under bash, with fake `nvidia-smi` and `rreport-runtime` executables on `PATH`, and every path is checked from outside, signals included. Each of its ten mechanisms was removed on purpose and caught by a test.
- **A pod whose own key RunPod refuses everywhere** cannot stop itself here either. It then waits, logging that it must be stopped from RunPod's console, rather than restarting in a loop. The runbook's first check on a real pod settles which key works.

## Alternatives considered

- **`exec` the runtime, and let RunPod restart it.** It is simpler, but a failure that repeats (and most do) repeats forever, billing. Rejected.
- **Exit and rely on RunPod's restart policy.** RunPod documents no restart limit or backoff. Rejected.
- **A supervisor such as `tini` as PID 1.** It would forward signals, but it could not act on the runtime's exit status, which is the point. Bash in a dozen tested lines does both.

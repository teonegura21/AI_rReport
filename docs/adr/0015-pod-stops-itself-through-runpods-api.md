# ADR-0015: An idle pod stops itself through RunPod's API, not runpodctl

- Status: Accepted
- Date: 2026-09-30

## Context

The runtime's dead-man switch stops the pod once nobody has needed it for `idle.stop_after` (the runtime design, "Idle stop"). Until now it did so by running `runpodctl stop pod $RUNPOD_POD_ID`, trusting that RunPod ships `runpodctl` in every pod with a pod-scoped key in `RUNPOD_API_KEY`. Research against RunPod's own sources (its docs repository and `runpodctl` at every tag, read on 2026-09-30) found that foundation shakier than it looked:

1. **Two command forms.** `runpodctl` 2.x (since 2026-02) uses `runpodctl pod stop <id>`, which calls REST v1. The old `stop pod` form still works, deprecated, and calls GraphQL's `podStop`. The docs now show only the new form.
2. **The pod's own key may be refused by REST.** RunPod never documents what the pod-scoped key may do. Two independent reports from 2026 say it gets `403` from REST (v2's `/pods/{id}/action`, and v1 through `runpodctl pod stop`), while GraphQL's `podStop` accepts it. RunPod's own test code says the opposite for other calls.
3. **The APIs are moving.** REST v1 retires on 2026-11-15. GraphQL is deprecated and retires in early 2027. REST v2 (`api.runpod.io/v2`) is the current API.
4. **`runpodctl` may be missing.** How it gets into a pod is undocumented. One custom image was reported without it.
5. **Exiting is not stopping.** RunPod restarts a container whose command exits (two reports), so the runtime cannot stop the pod by ending itself.

## Decision

The runtime stops its pod by calling RunPod's API itself, through `rr_runpod` (`RunpodApi::stop_self()`):

1. **REST v2 first**, with `POST /pods/{id}/action {"action": "stop"}`.
2. **GraphQL's `podStop` if REST refuses the key** (`401`/`403`), and only then. Any other failure is reported as it is.
3. **The key** is `idle.runpod_api_key` if set, else `RUNPOD_API_KEY`.
   - An owner whose pod key turns out to be too weak creates a key of their own and passes it as a RunPod secret.
   - `RUNPOD_API_KEY` is deliberately not an alias of the setting. RunPod always sets it, so an alias would refuse to start whenever the two differ.
4. **Precedence.** A configured `idle.stop_command` still wins (for machines other than RunPod). Without a key, idleness is only logged.
5. **Retries.** A failed stop is logged as an error and tried again every 5 minutes while the pod stays up and idle.

The hub's `PodManager` uses the same module with the owner's account key: REST v2 only.

## Consequences

- **What is proven locally, and what only a pod can prove.**
  - Proven: the whole path runs end to end in a component test, from the real runtime binary, idle, to the fake RunPod API. REST refuses the pod key, and GraphQL stops the pod.
  - Not proven: whether RunPod's real pod key is accepted anywhere is left to the M0 runbook's first check on a real pod.
- **No dependency on `runpodctl`** in our image, on its version, or on its command form.
- **Revisit before GraphQL retires** (early 2027). By then RunPod must either let pod keys use REST v2, or the runbook's owner-key path becomes the only one.
- **More code.** A small, tested HTTP client (`rr_runpod`) replaces a one-line command.

## Alternatives considered

- **`runpodctl pod stop` (the documented form).** It goes through REST v1, which retires in November 2026 and reportedly refuses the pod key. Rejected.
- **`runpodctl stop pod` (the legacy form, GraphQL).** It works with the pod key today, but it depends on `runpodctl` being present and on a deprecated command. Rejected as the only path. Its API call survives as our fallback.
- **Exit the runtime when idle.** RunPod restarts the container, so the pod keeps billing. Rejected.

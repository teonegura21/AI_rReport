# ADR-0001: Hub/runtime split

- Status: Accepted
- Date: 2026-09-29

## Context

- The owner has no local GPU. All model inference runs on a rented GPU pod (RunPod).
- **The pod is ephemeral.** It is stopped to save money and can die at any time.
- **YouTube blocks datacenter IPs** ("Sign in to confirm you're not a bot").
- **Legal risk.** 2026 court rulings treat circumventing YouTube's protections as a DMCA §1201 risk. A server that fetches YouTube is exactly the risky pattern.
- **The library must stay usable offline.** Reports, idea maps and research threads must be readable while the pod is off.

## Decision

The system is **two programs** with a strict division of responsibilities.

- **`rreport` (the hub)** runs on the user's machine: a Linux laptop today, a server tenant in the hosted product.
  - It is the **only source of truth**: the library database, all artifacts, all history.
  - It captures media from the user's own network (yt-dlp, the browser extension), orchestrates the pipeline, and serves the web UI and the MCP adapter.
  - It needs no GPU.
- **`rreport-runtime` (the runtime)** runs on the GPU pod.
  - It is **stateless** apart from caches with a time-to-live (TTL).
  - It serves models (speech, LLMs, embeddings) behind one authenticated HTTP API.
  - It never talks to YouTube.

The protocol between them is async and idempotent:
- content-addressed blobs;
- jobs keyed by the stage cache key;
- always-streamed model calls, because the RunPod HTTP proxy cuts silent requests after about 100 s.

## Consequences

- Stopping or losing the pod never loses data. A job simply resumes on the next pod.
- Capture happens from a residential IP, which avoids YouTube's datacenter blocks, and the hosted service never downloads from YouTube.
- The same hub binary becomes the multi-tenant hosted hub in v3 (one library per tenant, ADR-0008).
- Cost: every model call crosses the network. With streaming and small mission packets, the overhead is negligible next to generation time.
- The hub must stay awake while a job runs (it asks the OS not to sleep). Jobs are resumable anyway.

## Alternatives considered

- **Everything on the pod.** Simpler, but the library is offline whenever the pod is stopped, and capture from the pod's datacenter IP fails. Rejected.
- **Pipeline running on the pod, hub only for storage.** Fewer round trips, but it creates two sources of truth and needs state on a machine designed to die. Rejected.
- **Cloud APIs only, no pod.** It contradicts the requirement to run open models on owned compute. Kept only as an *optional* provider path.

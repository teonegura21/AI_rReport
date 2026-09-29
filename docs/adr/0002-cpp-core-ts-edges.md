# ADR-0002: C++ core, TypeScript at the edges

- Status: Accepted
- Date: 2026-09-29

## Context

- The owner's preferred language is C++, and the bar for code quality is very high.
- The heavy engines are already C/C++: llama.cpp, whisper.cpp, sherpa-onnx and ONNX Runtime.
- Some parts must run in a browser (the web UI, the extension). The Model Context Protocol (MCP) spec changes fast (big redesign in 2026-07) and has **no official C++ SDK**.

## Decision

- **C++23** for everything that is logic: capture orchestration, the pipeline, the Fidelity Ledger, agents, storage, retrieval, research, the runtime supervisor.
- **TypeScript** only where the platform forces it:
  - the web UI (React + Vite);
  - the browser extension (WXT, Manifest V3);
  - a *thin* MCP adapter built on the official SDK.

  These call the hub's REST/SSE API and contain no business logic.
- JSON Schemas are the single contract between the two languages. TypeScript types are generated from them.

## Consequences

- No Python in the product. The hub ships as a self-contained binary; the runtime as one Docker image.
- Full control over memory, latency and concurrency, and direct use of C/C++ engine APIs where needed.
- Iterating on prompts must not require recompiling: prompts and schemas are data files, embedded for release builds but loadable from disk during development.
- Fewer ready-made AI libraries (no LangChain equivalent). We build the few pieces we need (agent loop, retrieval, HTML extraction) ourselves, which also keeps them small and auditable.

## Alternatives considered

- **Python core.** Fastest iteration and the biggest ML ecosystem, but heavy to package and sell, with slower hot paths. Rejected by the owner.
- **C++ engine + Python agents.** Two runtimes to ship and debug. Rejected.
- **Native C++ MCP server.** Community SDKs are immature and would have to chase a changing spec. Rejected in favor of the thin TypeScript adapter.

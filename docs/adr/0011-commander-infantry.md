# ADR-0011: Commander and infantry: a native sub-agent environment

- Status: Accepted; in part superseded by [ADR-0017](0017-local-models-only.md): no hosted commander, no cloud specialists; a refusal or softening goes to the other local model family
- Date: 2026-09-29

## Context

- The owner chose **Qwen3.8** as the "big" model (the *commander*) for its price/quality.
- The owner chose **quantized Gemma 4** models running in parallel "like an army of infantry" for the small-model work.
- Small models are reliable only when each task is narrow, fully specified and checkable.

## Decision

The engine implements a native sub-agent environment in C++ (`rr_agents`).

| Concept | Meaning |
|---|---|
| **Mission** | A typed order: role, prompt template, input packet (2–8K tokens: the task's own slice of the video plus a ~1.5K-token *video brief*), output schema, validators, budget, priority, retry policy |
| **Squad** | Fan-out/fan-in: run N missions in parallel, validate each result, retry failures with the validator's message, gather results in order |
| **Dispatcher** | Priority queues per model endpoint, sized to the endpoint's parallel slots (llama-server continuous batching) |
| **Escalation** | Two failed validations → the commander. A refusal or softened output → the fallback provider |
| **Commander tools** | During research the commander calls `dispatch_squad(mission, inputs[])` itself |
| **Battle log** | Every mission is recorded (`missions` table) for the UI, cost accounting and the eval |

Role bindings are configuration:
- **commander** = Qwen3.8-27B on the pod, or a hosted Qwen3.8 API;
- **infantry** = Gemma 4 26B-A4B / 12B / E4B;
- **optional specialists** = Claude, GPT or Gemini.

## Consequences

- One mechanism powers ingestion's map-shaped stages and the research agent.
- Army size is bounded by attention-cache memory. The M0 capacity benchmark measures total infantry throughput at 1/4/8/16/32 slots per GPU profile.
- Known model risks are measured by a dedicated eval slice with fallbacks: Qwen censors China-related topics, and Gemma has safety tuning.

## Alternatives considered

- **One mid-size worker model for everything.** Simpler, but it loses parallel throughput and the commander's writing quality. Superseded by the owner's decision.
- **An external agent framework (LangGraph etc.).** Python-only, and it violates ADR-0002.

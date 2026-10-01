# ADR-0017: Every model call runs on the pod: local models only

- Status: Accepted
- Date: 2026-10-01
- Supersedes: plan §4.6 (optional cloud specialists); in part ADR-0001, ADR-0007 and ADR-0011 (their cloud and "fallback provider" paths, listed below)

## Context

The owner's direction (2026-10-01): rReport runs on the pod, powered by local AI only, with **no external model APIs**. The effort goes into getting the most out of the local models instead.

Until now the design kept cloud models as options in four places:
- plan §4.6: Claude, GPT and Gemini as optional specialists, with an Anthropic Messages client in M8;
- ADR-0011: a hosted Qwen3.8 API as the commander, and cloud specialists per role;
- ADR-0007 and ADR-0011: a refused or softened answer goes to "the fallback provider", which in practice meant a cloud model;
- ADR-0001: cloud APIs "kept only as an optional provider path".

Local only is the better product:
- **Privacy.** The videos a user studies, and the research they attach to them, never reach a model vendor.
- **Reproducibility.** The weights are pinned by checksum (`docker/runtime/catalog.toml`) and llama.cpp by commit. A result can be reproduced a year later. A hosted model can change behind the same name without notice.
- **Cost.** A job costs pod time and nothing else, and the eval measures it exactly (ADR-0019).
- **One system to measure.** The eval compares configurations of a stack we control, not a moving target.

What it costs: the local models (Qwen3.8-27B, Gemma 4) are weaker than frontier APIs. This decision accepts that and invests in test-time compute (ADR-0018), closed-world bookkeeping (ADR-0005) and measurement (ADR-0019) instead.

## Decision

1. **All inference runs on the pod.** The commander, the infantry, the pipeline's judges and the eval's judges are served by `rreport-runtime`, from catalog weights.
2. **The hub has one model endpoint: the runtime.** That is `runtime.url`, or RunPod's proxy URL for `runpod.pod_id`.
   - Providers for real work are built in one place, `ModelStack` (P4), and only against that endpoint. A test checks every role's provider.
   - No configuration key names another model endpoint. `rreport doctor` states where model calls go.
   - The runtime relays only to its own engines on 127.0.0.1 (ADR-0003). Its own outbound traffic is the model files (Hugging Face, checksummed) and RunPod's API (stopping itself, ADR-0015/0016): no inference.
   - A remote model endpoint of any kind needs a new ADR.
3. **A refusal or a softened answer falls back to the other local family.**
   - Qwen (the commander) and Gemma (the infantry) come from different vendors, with different safety tuning, so one often answers what the other declines.
   - A mission whose answer is classified as a refusal or a softening is tried once on the other family. If that fails too, the item is **published as a finding** ("both local model families declined this passage"), never dropped or quietly softened.
   - This replaces "`refused` → the fallback provider" (ADR-0007) and "a refusal or softened output → the fallback provider" (ADR-0011).
4. **Refusals are recognized by what the answer does, not by what the server says.** Constrained decoding is the reason. A model that would refuse in prose instead returns *valid but evasive* JSON: sentences labelled filler, an empty atom list, a paraphrased quote. So the signals are the ledger's checks (every sentence accounted for, must-quotes verbatim, numbers and names present) and the judges' softening flags. llama-server never reports `content_filter`.
5. **Data sources are not model calls.** Fetching captions, and later Wikipedia or web search for research (M9), stay allowed. Each is opt-in and listed by `rreport doctor`. None of them runs a model on the user's behalf.

## Consequences

- **Removed from the design:**
  - the Anthropic Messages client (plan §4.6, M8);
  - the hosted Qwen3.8 commander (ADR-0011, plan §3.6 and §4.4);
  - cloud specialists per role.
- **Kept, because it is local-capable:** the provider's plain OpenAI-compatible dialect (`Dialect::openai`). The plan allows vLLM or SGLang as infantry engines on the largest pods (§4.5), and they speak plain OpenAI. They run behind the runtime like llama-server does.
- **Refusals cannot be bought away.** The eval's blunt videos (plan §10: blunt speech, China-sensitive topics) measure how often both families decline, and each such case is visible in the article's Fidelity Report.
- **Quality comes from structure and compute.** The ceiling is set by local models. ADR-0018 decides how much compute to spend per job; ADR-0019 decides whether a change is an improvement.
- **The earlier ADRs stay as they were written** (ADRs are immutable). Each of them is marked "in part superseded by ADR-0017":
  - ADR-0001: Alternatives, "kept only as an optional provider path";
  - ADR-0007: Retry policy, "`refused` → the fallback provider";
  - ADR-0011: Escalation, "→ the fallback provider"; role bindings, "or a hosted Qwen3.8 API" and "optional specialists".

## Alternatives considered

- **Cloud fallback for refusals only** (the previous design). It would send exactly the most sensitive passages to a third party, the ones a model declined. Rejected by the owner.
- **A hosted API serving the same open weights.** The model is the same, but the data leaves, the cost is per token, and the provider can change the model's quantization or template. Rejected.
- **A frontier model as the eval's judge only.** A stronger judge is tempting, but the eval would then depend on an external model that changes over time. Frozen, human-calibrated local judges (ADR-0019) give a measure that stays comparable. Rejected.

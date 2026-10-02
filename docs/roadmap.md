# Roadmap: the intelligence core (M1 and M2)

M0 gave rReport its foundations: the pod's image and runtime, the hub's `doctor`, `pod` and `bench`, and model calls. M1 and M2 build its intelligence core on them:
- **M1**, the vertical slice: video → transcript → closed-world atoms → article → verification → HTML;
- **M2**, the evaluation harness that measures it.

Everything runs on local models (ADR-0017), at the quality-first `max` tier by default (ADR-0018). The method is measured (ADR-0019) and resumable (ADR-0020).

The work is cut into thirty phases, each one commit that builds, passes CI and is mutation-tested on its own. Design notes and code comments cite them as "plan P13" and so on: this page is where those references lead. [plan.md](plan.md) keeps the product plan as it was approved. What changed since is recorded in ADRs and design notes, not by editing it.

## Phases

| Phase | What | Status |
|---|---|---|
| **0. Decisions** | | |
| P0 | ADRs 0017–0020; eval candidate videos (CC-BY or US public domain) | done: `d4cd7d6`, `226aeb0`. The candidates await the owner's approval |
| **1. Model calls that can carry test-time compute** | | |
| P1 | Schemas reach the model with their fields in the order written | done: `5c41371` |
| P2 | Sampling presets from the model cards, a thinking budget, no penalties | done: `49b6a62` |
| P3 | `n` choices from one reading of the prompt | done: `d893d1f` |
| P4 | `Limiter::acquire(k)`, `ModelStack` (the egress rule), the call memo | done: `9b17a36`, `9853e09` |
| P5 | Engines report their context; the commander gets four shared slots → **runbook R1** | done: `98e1aee` |
| P6 | Nightly L4: the fake engine checked against a real llama-server | done: `1a67952` |
| **2. The army and its playbook** | | |
| P7 | `rr_prompt`: prompts as source code ([prompts.md](design/prompts.md)) | done: `3a69573` |
| P8 | `rr_agents`: the army, its ladder, ceilings and battle log ([agents.md](design/agents.md)) | done: `fae40e1` |
| P9 | `rr_reason`: votes, waves, judgements, the tiers ([reasoning.md](design/reasoning.md)) | done: `80998bf` |
| P10 | The oracle model, fault injection, mutation testing in-tree ([testing.md](design/testing.md)) | done: `cf4a4b8` |
| **3. Text, transcript, ledger, pipeline** | | |
| P11 | `rr_text`: sentences, numbers by value, names, quote alignment, BM25; fuzzing ([text.md](design/text.md)) | done: `d5ccefc` |
| P12 | `rr_knowledge`: the knowledge model's types, artifact schemas, the `[A#]` marker parser, fuzzed ([knowledge.md](design/knowledge.md)) | done: `d32213c` |
| P13 | `rr_fidelity`: the deterministic ledger checks, each with a repair message; the oracle's faults caught ([fidelity.md](design/fidelity.md)) | done: `3f78d77` |
| P14 | `rr_media`, `rr_transcript`: captions → `S#` sentences with timings; auto captions re-punctuated by projection ([transcript.md](design/transcript.md)) | done: `3574dbf` |
| P15 | `rr_pipeline`: stages, the executor, the stage cache; the stages `source` and `transcript`, with the first prompt, `punctuate`; `rreport ingest / resume / jobs` ([pipeline.md](design/pipeline.md)) | done: `9d1b971` |
| P16 | `rr_eval`: the set, snapshots (`eval fetch`), gold anchored to the timeline, silver drafted by the `annotate` stage (`eval annotate`), the review sheet (`eval sheet`) ([eval.md](design/eval.md)) → **runbook R2** | done: `39945d3` |
| **4. The vertical slice** | | |
| P17 | The stage `outline`: sections from chapters, from the commander's outlines agreed on, or from pauses, none over eight minutes; each described, the video's brief joined; the prompts `outline`, `describe`, `brief`; windows that fit their engine, shared with the eval ([pipeline.md](design/pipeline.md#the-outline-stage)) | done: `6d2db41` |
| P18 | The stage `atomize`, Ledger A: closed-world windows read by the infantry, three samples voted on sentence by sentence at `max`, the commander settling what the vote does not, a recall pass where atoms hold little; every answer checked as the ledger is, and repaired; the prompt `atomize` in three modes ([pipeline.md](design/pipeline.md#the-atomize-stage)) | this commit |
| P19–P20 | The stages `plan`, `write` (with seams): prompt, schema, stage, oracle support, fault tests | |
| P21 | `rr_report`, `rreport report`: the HTML article | |
| P22 | End to end through the real executables; `kill -9` and resume → **runbook R3** | |
| **5. Measurement** | | |
| P23 | `eval run / score / export` | |
| P24 | Frozen eval judges: key-fact recall, supported sentences, pairwise flow | |
| P25 | Statistics and the gate: `eval compare / gate` → **runbook R4** | |
| P26 | `eval report`, `eval sweep` | |
| P27 | `eval label / calibrate` → **runbooks R5, R6** | |
| **6. Extras, kept only if the eval shows a gain** | | |
| P28 | Logprob confidence driving adaptive waves | |
| P29 | Self-MoA fusion of the commander's drafts; MTP speculative decoding | |

## What only your pod can settle

Some questions need real GPUs and real models. Each runbook says what to run and what to send back:

| Runbook | After | What it settles |
|---|---|---|
| R1 | P5 | Commander slots, unified KV and context, on 48 and 80 GB pods ([deploy/runpod](../deploy/runpod/README.md)) |
| R2 | P16 | The eval set: your approval of the candidates, then silver annotations corrected into gold |
| R3 | P22 | M1's acceptance: three videos ingested at `max`; 100% of sentences accounted for, atom coverage ≥ 97%, no invalid marker, every must-quote verbatim, numbers ≥ 99% |
| R4 | P25 | Tiers compared with replicates and an A/A noise floor |
| R5 | P27 | Forty labels that calibrate the eval judges |
| R6 | P27 | Sweeps and the gate on the test split: M2's acceptance, key-fact recall ≥ 90% |

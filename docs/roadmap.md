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
| P18 | The stage `atomize`, Ledger A: closed-world windows read by the infantry, three samples voted on sentence by sentence at `max`, the commander settling what the vote does not, a recall pass where atoms hold little; every answer checked as the ledger is, and repaired; the prompt `atomize` in three modes ([pipeline.md](design/pipeline.md#the-atomize-stage)) | done: `c07666c`; the job ceilings per hour of video it calls for ([pipeline.md](design/pipeline.md#the-army-raised-when-needed)): `08c74ea` |
| P19 | The stage `plan`: each section planned by the commander, thinking, in a closed world, in parts where it holds less; every answer checked and repaired, a duplicate held to lose nothing; what no answer planned told in the order said; the prompt `plan` ([pipeline.md](design/pipeline.md#the-plan-stage)) | done: `0d6f50c` |
| P20 | The stage `write`: each section written by the commander from its plan, without thinking, the tier's drafts each, in parts where it holds less; every draft checked as Ledger B checks a section, and sent back with what failed; of those that pass, the fewest warnings kept; what no draft passed told as its atoms say it; the article checked whole, the plan's duplicates excused; the prompt `write`, answered in prose ([pipeline.md](design/pipeline.md#the-write-stage)). Then the judge that picks among the drafts, and the seams between sections: the paragraphs where sections meet rewritten together, each keeping its atoms, kept where the judge reads no more issues ([pipeline.md](design/pipeline.md#the-seams-stage)) | P20a, the stage: `908d157`; P20b, the judge: `f76ae53`; P20c, the seams: `cba36f3` |
| P21 | `rr_report`, `rreport report`: the article as one web page, each sentence starting the video where it was said, its markers linked to the ledger; the Fidelity Report: what was kept, against M1's targets, what was left out and why, what the stages could not do; rendered from the artifacts whenever asked, never stored ([report.md](design/report.md)) | done: `2426850` |
| P22 | End to end through the real executables: every stage at `max` against fake engines that answer as the oracle; a job killed with SIGKILL anywhere and resumed, asking again only what its memo did not keep, to the same article ([testing.md](design/testing.md#end-to-end-killed-and-resumed)) → **runbook R3** | done: `c5ffd9e` |
| **5. Measurement** | | |
| P23 | `eval run / score / export`: the set's videos through the article stages, each replicate a job of its own salt, a run taken up where it stopped; each article scored by code, the Fidelity Report's figures by the page's own code, gold's quotes, terms and facts cited, its length, what it cost; the scores kept, and exported as CSV or JSON ([eval.md](design/eval.md#runs-and-scores)) | done: `af96f9b` |
| P24 | Frozen eval judges: a panel pinning its prompts, its judges' model files and its procedure (`eval/judges/eval-v1.toml`); each article's facts and sentences read by both families, against the video's own words, the evidence quoted and checked; a judgement a job of its own, never read twice unchanged; scores kept beside the code's, reported only until calibrated (P27). Flow, pairwise, comes with P25's `eval compare`, which pairs runs; its prompt is pinned already ([eval.md](design/eval.md#the-judges)) | done: `e023f00` |
| P25 | Statistics and the gate: two runs compared video by video, each metric's difference with its paired-bootstrap interval, its standard error and what the videos could see; a candidate gated against its baseline by a reviewed file: invariants on every entry, a non-inferiority margin a metric, inconclusive failing; the margins checked against an A/A run under other salts (`--first-replicate`); then the articles of two runs read pairwise for flow ([eval.md](design/eval.md#comparisons-and-the-gate)) → **runbook R4** | P25a, compare and gate: `57902e2`; P25b, pairwise flow: `5754051` |
| P26 | Sweeps and the report: tier variants named by what they change (`max+write.thinking=true`), accepted wherever a tier is; a sweep file of variants, each with why, run against one baseline under the same salts on the dev split, and what each moved; every run on one page, quality against minutes per video-hour with each mean's interval, the runs, each run's videos ([eval.md](design/eval.md#sweeps-and-the-report)) | done: `f2611e7` |
| P27 | Labels and calibration: items the panel read drawn uniformly for a person to label, blind, shown as the judges saw them, a part's two versions in a drawn order; each judge against the labels, kind by kind: Cohen's κ at 0.4, each class's precision and recall above a random judge's by their Wilson lower bounds; position and length bias ([eval.md](design/eval.md#labels-and-calibration)) → **runbooks R5, R6** | done: this commit |
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
| R4 | P25 | Tiers compared with replicates, and the gate's margins checked against an A/A run |
| R5 | P27 | Forty labels that calibrate the eval judges |
| R6 | P27 | Sweeps and the gate on the test split: M2's acceptance, key-fact recall ≥ 90% |

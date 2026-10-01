# ADR-0019: How the eval measures, and when a change may merge

- Status: Accepted
- Date: 2026-10-01

## Context

Plan §10 gates every model or prompt change: "a change merges only if no metric regresses". Read literally, that rule cannot work:
- **The pipeline is stochastic.** It samples (ADR-0018), and a GPU's batching changes results slightly from run to run. Two runs of the same configuration differ, so an unchanged pipeline "regresses" on noise alone: half the time on one metric, nearly always on one of several.
- **The set is small, and its videos differ.** M2 has 8–10 videos, growing to about 24, and a lecture and a podcast differ far more than two runs of either. The items inside one video (sentences, facts) are correlated. Resampling them as if independent would claim a precision the data does not have (Miller 2024).
- **LLM judges are biased.**
  - they prefer the answer shown first (position: Wang et al. 2023);
  - they prefer the longer answer (length);
  - they prefer their own model's text (self-preference: Panickssery et al. 2024).

  A judge that is tuned along with the pipeline can also be gamed by it: the measure becomes a target.
- **The gold must outlive the code.** A new sentence splitter renumbers every `S#`, so gold keyed by sentence would be orphaned by an improvement.

## Decision

### Data
- **Sets** (`eval/sets/*.toml`):
  - only videos licensed CC BY or in the US public domain, with attribution;
  - each video records its type, its caption kind (manual or automatic), and its split (`dev` or `test`).
  - The four types: **talk or lecture**, **conversation** (podcast, interview, panel), **visual tutorial** (slides, code, whiteboard), **narrative** (documentary, news).
  - Development iterates on `dev`. `test` is used only for gates and milestone acceptance.
- **Snapshots** (`eval/data/<id>/`): the captions and metadata, pinned by hash. A video that changes or disappears on YouTube does not change the eval.
- **Gold** (`eval/gold/<id>.toml`), anchored to the timeline (ADR-0009):
  - **facts** `(t0_ms, t1_ms, quote)`, each `core` or `detail`;
  - must-quotes;
  - terms.

  The commander drafts *silver* (`rreport eval annotate`), and a person corrects it into gold. The file records its reviewer, and `eval score` refuses gold that nobody reviewed.
- **Labels** (`eval/labels/`): 30–50 human verdicts on article sentences, and pairwise flow preferences. They calibrate the judges (below).

### Metrics, in three classes
1. **Invariants:** they must hold on every video and every run.
   - sentence accounting 100%;
   - no invalid markers;
   - must-quotes 100%.
2. **Deterministic metrics:**
   - atom coverage;
   - numbers correct by value ("thirty-seven percent" = 37%);
   - length ratio;
   - operations: GPU-minutes, tokens, dollars.
3. **Judged metrics:**
   - key-fact recall;
   - supported-sentence precision;
   - misattribution, softening and intensifying rates;
   - flow, judged pairwise against the baseline.

### Judges
- **The eval's judges are frozen and separate.** They are a panel (`eval/judges/eval-v1.toml`): the model by checksum, the prompts by hash, the sampling and the seeds.
  - They are never the pipeline's own judges, so improving the pipeline cannot game its measure.
  - A changed panel is a new version. Scores from two panel versions are never compared.
- **Cross-family.** The panel's models come from a family other than the writer's. The commander writes (Qwen), so the panel is Gemma.
- **Evidence first.** A judge quotes its evidence, then reasons, then gives its verdict; field order is enforced by the schema (plan P1).
- **Both orders.** A pairwise comparison runs in both orders. A preference that flips with the order is a tie.
- **Calibrated before trusted.** A judged metric is *report-only* until its judge agrees with the human labels:
  - Cohen's κ ≥ 0.4;
  - per class, the Wilson 95% lower bounds of precision and recall beat a random judge with the same positive rate.

  The calibration report also measures position bias (the flip rate) and length bias.

### Statistics
- **The unit is the video.**
  - Each configuration runs **≥ 3 replicates** per video, and a video's score is their mean.
  - A comparison works on per-video deltas, candidate − baseline.
- **Common random numbers.** Replicate *r* salts the seed of every sampled call (ADR-0020), and the candidate and the baseline use the same salts.
  - Stages the candidate did not change are cache hits with identical outputs (ADR-0004).
  - So the delta measures the change, not the dice, and costs only the changed stages.
- **Paired bootstrap over videos:** 10,000 resamples of the per-video deltas give the 95% interval of the mean delta.
- **A/A runs.** The baseline is compared with itself under disjoint salts. Nothing is shared, so this is the pipeline's full run-to-run noise: an upper bound on the noise of a comparison that shares stages. The A/A run serves three purposes:
  - it sets the margins (below);
  - it checks the statistics: its interval must cover 0;
  - it exposes unseeded randomness: A/A deltas larger than the replicates' spread predicts.

### The gate
Every metric is oriented so that higher is better: an error rate enters as its negative. A change passes when both hold:
1. **Every invariant** holds on every video and replicate.
2. **Every gated metric** *m* passes: the lower bound of its 95% interval for candidate − baseline is ≥ −δ<sub>m</sub>. Here δ<sub>m</sub> is the regression the gate tolerates on *m*, declared in `eval/gates/*.toml`.

This is a non-inferiority test, with two error rates to control:
- **A real regression larger than δ<sub>m</sub> passes at most 2.5% of the time.** The interval guarantees it, whatever the noise.
- **An unchanged pipeline should pass.** With per-metric standard error SE, it passes one metric with probability Φ(δ/SE − 1.96): about 50% at δ = 1.96 SE, about 80% at δ = 2.8 SE.
  - Margins below the noise would make the gate a coin toss. So the margins are set from the A/A run, for the whole gate at once: the bootstrap over the A/A deltas estimates how often an unchanged pipeline passes every gated metric, and that must be at least 80%.
  - A margin is never loosened at gate time. A comparison too noisy to show non-inferiority fails as *inconclusive* (its interval spans both 0 and −δ), and the remedy is more replicates or videos. A candidate whose whole interval lies below 0 fails as *worse*.
- **Judged metrics join the gate only once calibrated.** Until then they are reported, never gating.
- **Operations metrics are reported, not gated.** Quality comes first (ADR-0018).

`eval compare` reports, for every metric:
- the delta and its interval;
- δ and SE;
- the smallest improvement the set can detect: 2.8 SE, at 80% power and one-sided α = 2.5%.

## Consequences

- **A gate decision is a statement with known error rates,** not a comparison of two numbers.
- **Eval cost is real:** about 9 video-hours × tiers × replicates.
  - Upstream stages are shared through the cache.
  - Sweeps run on `dev` with 2 replicates.
  - `test` is used only for gates and milestones.
- **Human time:**
  - correcting silver into gold: about 3–4 hours (runbook R2);
  - labelling for calibration: about 1 hour (R5).
- **Small sets can detect only large effects,** and the report says how large. Nobody reads noise as signal.
- **The M1 and M2 acceptance thresholds stay absolute** (plan, runbooks R3 and R6); for example, key-fact recall ≥ 90%. The gate is for *comparing* configurations.

## Alternatives considered

- **"No metric regresses", read literally.** Noise alone fails it, and it cannot say how large a regression it lets through. Rejected.
- **Resampling sentences or facts instead of videos.** It understates the variance, because items within a video are correlated. Rejected.
- **The pipeline's own judges as the measure.** Optimizing against your own judge games it. Rejected.
- **One order per pairwise comparison.** It measures position as much as quality. Rejected.
- **ROUGE, BERTScore and similar overlap scores.** They track faithfulness and coverage poorly for long, restructured text. They may be reported, never gated.
- **A frontier API as judge.** It is excluded by ADR-0017, and it would change under us.

## Sources

- Koehn: *Statistical Significance Tests for Machine Translation Evaluation* (EMNLP 2004): the paired bootstrap.
- Miller: *Adding Error Bars to Evals: A Statistical Approach to Language Model Evaluations* (2024): clustered errors, paired differences, resampling.
- Wang et al.: *Large Language Models are not Fair Evaluators* (2023): position bias, the swap.
- Panickssery, Bowman, Feng: *LLM Evaluators Recognize and Favor Their Own Generations* (2024).
- Verga et al.: *Replacing Judges with Juries: Evaluating LLM Generations with a Panel of Diverse Models* (2024).

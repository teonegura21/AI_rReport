# ADR-0018: Compute tiers, quality first, and safety ceilings

- Status: Accepted
- Date: 2026-10-01

## Context

The owner asked for the most intelligence the local models can give (ADR-0017), and chose **quality first**: spend the compute that makes the article better; show the cost, but do not cap it.

Test-time compute is how a fixed model gets better answers. What works, by the evidence (sources at the end):
- **Sampling several candidates and selecting** with external checks. Self-consistency votes beat LLM verifiers at equal compute (Singhi et al. 2025).
- **Adaptive allocation:** more samples only where the samples disagree. It is more than 4× as efficient as uniform best-of-N (Snell et al. 2024).
- **Thinking for planning and selection, not for drafting.** Reasoning models hallucinate more when they summarize: on Vectara's leaderboard DeepSeek-R1 scored 14.3% against DeepSeek-V3's 3.9%.
- **Revising with external feedback** (a checker, the evidence) works. Unguided self-refinement does not (Huang et al. 2024; CRITIC; Chain-of-Verification).

Each of these multiplies GPU time, and each can run away:
- Gemma 4 is known to loop while thinking;
- Qwen3.8's default reasoning effort over-thinks;
- a repair loop may never converge;
- samples may keep disagreeing.

The eval must chart quality against cost (ADR-0019), which needs a few named compute settings rather than a continuum.

## Decision

1. **Three tiers: `fast`, `thorough` and `max`.**
   - Each tier is a file, `config/tiers/<tier>.toml`. Per stage, it sets:
     - the samples and their waves (first, step, maximum, quorum);
     - thinking on or off, its budget, and the reasoning effort;
     - the repair rounds;
     - which judges run.
   - **`max` is the default.** A job records its tier.
   - Each stage's cache key includes its own section of the tier, in canonical form (ADR-0004). Changing a tier re-runs exactly the stages whose settings changed, and the stages downstream of them.
   - Every stage takes its compute from the tier, with `fast` as the degenerate case (one sample). The same code path is thus tested at every tier.
   - The initial tiers:

   | Stage | `fast` | `thorough` | `max` |
   |---|---|---|---|
   | outline and brief (commander, thinking) | the video's chapters, else 1 sample | 1 sample, budget 8K | chapters, else 3 samples with boundary consensus, budget 16K |
   | atomize (infantry, closed-world windows) | 1 sample + validators + repair | + a recall pass on weakly covered sentences | 3 samples, per-sentence vote; contested sentences → the commander, thinking |
   | plan (commander, thinking) | deterministic grouping | global plan + closed-world atom → paragraph assignment | the same, larger budget |
   | write (commander, thinking off) | 1 draft, 1 repair round | 2 drafts, the better one by judge, 2 rounds | 4 drafts → ledger score → evidence-first judge on the top 2 → pick → up to 3 rounds of targeted repair |
   | seams | — | boundary paragraphs rewritten | the same, re-checked |

2. **Quality first: no cost cap by default.** Cost is measured and shown per stage and per job:
   - GPU-minutes, as wall-clock on the pod;
   - tokens: prompt, cached, completion;
   - dollars, at the pod's price.

   It appears in the article's report and in `rreport jobs`.
3. **Safety ceilings stop runaway loops, not quality.**

   | Scope | Ceiling |
   |---|---|
   | A call | `max_tokens`, and a thinking budget (llama-server's `reasoning_budget_tokens`). At the budget the server closes the thinking with a short message, and the model answers. A call that still ends without an answer (`length`) counts as invalid, and its retry runs with thinking off |
   | A stage | the tier's repair rounds and maximum waves |
   | A job | completion tokens and wall-clock time, each per video-hour (a 3-hour podcast may spend 9× a 20-minute talk). Initially 5 million tokens and 4 hours per video-hour: roughly 5–10× the `max` tier's estimate. Runbook R3 measures the real cost, and the defaults follow it |

   - **On a job ceiling, the job stops spending.** It finishes what needs no model (accounting, the report) and publishes what it has, with a finding that says which ceiling was hit, where, and what was left undone. A ceiling is never a silent downgrade.
   - The user may lower any ceiling, for a budget; the eval counts the jobs that hit one.
4. **The thinking policy, by default:**
   - **on**, with a budget: outline, plan, selecting and judging, escalations;
   - **off**: drafting the article, and atomization's samples (narrow, schema-bound labelling).

   The eval A/B-tests drafting with thinking on (runbook R6). The data, not this ADR, decides whether it changes.
5. **Revision is targeted.** A repair round sends the writer only the sentences that failed, with their checker's or judge's reason and their evidence. A whole section is never sent back with "improve this".

## Consequences

- **The default job costs more than the plan's estimate** (15–30 pod-minutes per video-hour). The first real figure comes from runbook R3. The eval's quality-against-GPU-minutes chart lets the owner move the default with numbers.
- **The worst case is bounded:** no input can make a job run forever or spend without limit, and every ceiling hit is visible.
- **Every stage has a compute knob,** so a sweep can find where extra compute stops paying off (runbook R6).
- **More samples need more slots at once.** Hence `Limiter::acquire(k)`, `n` choices per request, and 4 commander slots with unified KV cache on the 48 and 80 GB profiles (plan P3–P5).

## Alternatives considered

- **A cost cap per job by default.** The owner chose quality first. The ceilings guard against bugs and loops, not against spending; a user may still lower them.
- **One fixed configuration.** It cannot chart quality against cost, and every development run would pay full price. Rejected.
- **Uniform best-of-N everywhere.** It spends where samples already agree. Rejected for adaptive waves.
- **Thinking on for every call.** It costs more, and in summarization it has been seen to add hallucinations (Vectara). For our models and our drafting, the eval's A/B test decides (decision 4).

## Sources

- Snell, Lee, Xu, Kumar: *Scaling LLM Test-Time Compute Optimally can be More Effective than Scaling Model Parameters* (2024).
- Singhi et al.: *When To Solve, When To Verify: Compute-Optimal Problem Solving and Generative Verification for LLM Reasoning* (2025).
- Huang et al.: *Large Language Models Cannot Self-Correct Reasoning Yet* (ICLR 2024).
- Gou et al.: *CRITIC: Large Language Models Can Self-Correct with Tool-Interactive Critiquing* (ICLR 2024).
- Dhuliawala et al.: *Chain-of-Verification Reduces Hallucination in Large Language Models* (2023).
- Vectara: hallucination leaderboard (HHEM), DeepSeek-R1 against DeepSeek-V3 (January 2025).

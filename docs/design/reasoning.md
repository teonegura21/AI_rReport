# Reasoning: many answers, one decision

The models are local and fixed (ADR-0017). Asking a model once is the least it can do. With more compute it can do better, but only if the extra answers are combined well. `rr_reason` holds the combining rules: pure functions with no I/O, no model and no threads, so each rule is exact and unit-tested. The tiers that say how much each stage spends live here too ([ADR-0018](../adr/0018-compute-tiers-and-ceilings.md)).

## Pieces

| Where | What it is |
|---|---|
| `rr/reason/vote.hpp` | Votes per item (`vote_keyed()`); a lone sample settling its item only as sure as the waves ask (`Waves::sure`, P28); waves of samples only where they disagree (`Waves`, `next_wave()`); the medoid text |
| `rr/reason/boundaries.hpp` | Where several outlines agree a section starts |
| `rr/reason/pairwise.hpp` | A judge's preference, counted only if it survives swapping the order |
| `rr/reason/select.hpp` | The best candidate by criteria in order of importance |
| `rr/reason/tier.hpp` | The tiers: each stage's compute, strictly read, canonical for cache keys |
| `config/tiers/{fast,thorough,max}.toml` | The tiers themselves, compiled in |

## The evidence, and the rules it gives

| Finding | Rule |
|---|---|
| Self-consistency, majority voting over samples, beats a single answer (Wang et al., 2023), and beats LLM verifiers at equal compute (Singhi et al., 2025) | Samples are voted on, not judged |
| Allocating samples adaptively, where they disagree, is more than 4× as efficient as uniform best-of-N (Snell et al., 2024); stopping once the samples so far agree saves up to 7.9× the samples, at an average accuracy cost under 0.1% (Aggarwal et al., 2023) | Waves: draw more only while an item is unsettled *and could still settle* |
| A model's own token probabilities say how sure it was; stopping or filtering samples by them saves most of their tokens at little cost (Fu et al., 2025, DeepConf) | A lone sample settles its item only when the tokens that chose its answer were as likely as the tier asks (`sure`) |
| Judges favour the first (or second) position, longer answers, and their own family (Wang et al., 2023; Zheng et al., 2023; Verga et al., 2024) | Every pairwise judgement in both orders; a disagreement is a tie |
| Fused answers can invent what no sample said | The medoid, a real answer that the others resemble most, not an average of answers |
| Fidelity failures are not bought back by style | Lexicographic selection: no weighted sum |

## Votes per item

A sample answers for many items at once: the 25 sentences of a transcript window. Its samples agree on most, so the vote is per item (`vote_keyed(items, ballots, quorum)`):

- **Settled** means the leading answer holds at least `quorum` of the votes cast on the item, and strictly more than any other answer.
- **A tie is never settled.** The order the samples came in must not decide.
- **An item no ballot is cast on is contested.** In a closed world (ADR-0005) every sentence is accounted for, so silence is not consent.
- **Votes are counted on canonical values.** A stage turns a sample's labels for an item into one string (canonical JSON), so two samples agree exactly when their labels do.
- **Ballots that cannot be counted are refused**, not dropped: one on an item not voted on, or a second ballot of one sample on one item.

**Quorums are shares, written as decimals.** Two thirds is `0.66`, since `0.67` is more than two votes of three. Floating point is given a little slack: `0.56 × 25` is 14.000000000000002, yet 14 votes of 25 meet a quorum of 0.56 (tested).

### Waves

`Waves{first, step, max, quorum}` says how many samples a decision gets. The first wave draws `first`. Another `step` are drawn while some item is unsettled and could still settle, up to `max` in all. "Could still settle" is checked: if even every remaining sample agreeing with an item's leader would not reach the quorum, drawing them is futile, and the item goes straight to escalation. 1–1–1 with one sample left can never make two thirds of four votes.

Atomize draws its waves (P28): a window whose readings leave a sentence unsettled, and that more samples could settle, is read `step` more times, up to `max`, the window's other sentences voted on again with every reading. A wave's samples are drawn from the window's own seed on, after those drawn (sample j draws seed + j), so that waves draw exactly the samples one request for them all would: a variant that stops early shares its first samples with the one that does not, and their comparison measures the stopping, not the dice. A window no answer read gets no wave: the same question would get no better answer. What is still unsettled after the last wave goes to the commander, thinking, at `max`.

At the `max` tier, atomize draws three samples at once, and no further wave. The eval's sweep for runbook R6 (`eval/sweeps/r6.toml`) measures what the three samples and the commander's rulings each buy, and two ways to spend fewer samples: `atomize-waves` draws two and a third only where they disagree; `atomize-sure` draws one, a second where the infantry was unsure of a sentence, and a third where those two disagree.

### How sure a lone sample was

One sample alone is a vote of one, and settles nothing by agreement: with a quorum, one ballot of one always "wins". A model can say how sure it was, though. Asked (`ChatRequest::token_probabilities`), llama-server sends each token's probability after its sampler chain: the chance that the same chain, from the same prefix, draws the same token again ([llm.md](llm.md#how-sure-the-model-was-token-by-token)). How sure a sample was of an item's answer is the product of the probabilities of the tokens that chose it, each token once: for an atomize answer, the sentence's filler label, each citing atom's kind, modality and must-quote-or-not, an "also" naming it, and whether its row has another atom (`confidence_of()`, [pipeline.md](pipeline.md)). The atoms' words are not part of it: free text never agrees, and the vote never asks it to.

The rule (`vote_keyed(items, ballots, quorum, sure)`):
- **A lone ballot settles its item only when its confidence reaches `sure`.** Otherwise the item is contested, and the waves draw another sample where one more could settle it (`next_wave()` reads settledness from the vote's winner, so the rule carries through).
- **A confidence not known is no confidence.** A byte whose token's probability never came, or a sample from a server that sent none, is unsure: the safe side, which spends a sample rather than trusts a guess.
- **Two samples or more are voted on, their confidences aside.** Samples that agree are evidence a sample's own probability is not: a model can be confidently wrong, the same way twice only by chance.
- **`sure` 0, every shipped tier's, lets every lone ballot settle**, as before P28. A slack of 1e-9 keeps a confidence at the bar at it: 0.3 × 3 is 0.8999999999999999.

So the waves become: one sample where the model is sure, more where it is not. Adaptive-Consistency spends samples where they still disagree (Aggarwal et al., 2023); DeepConf stops or filters samples by the model's own token confidence (Fu et al., 2025). Here a lone sample's own confidence decides whether it needs a second, and agreement decides the rest. Whether that spends GPU time better than three samples is the eval's to say: `atomize-sure` in runbook R6's sweep, adopted only if it loses nothing that matters for what it saves.

### The medoid

A settled item may carry text: an atom's wording, a section's title. Of the winning ballots' texts, the medoid is the one with the least summed distance to the others: a real answer, chosen because the others resemble it. Averaging or fusing answers would invent a new one, which no sample vouched for. The default distance is word-level Jaccard: lowercased words, with UTF-8 letters kept whole. Ties go to the first.

## Boundaries

At `max`, the commander outlines a video three times, and outlines rarely start a section on the same sentence. `boundary_consensus(outlines, tolerance, quorum)` keeps the boundaries enough outlines place within `tolerance` sentences of each other:
1. Every proposed position is a candidate. Its support is the number of outlines with a boundary that near; its spread, those boundaries' summed distance from it.
2. Candidates are taken in order: most support, then least spread, then earliest.
3. A candidate is kept when it has enough support and no kept boundary is within the tolerance of it.

So each place where outlines agree gives one boundary, at its most central proposed position. A boundary only one outline of three proposes is dropped.

## Pairwise judgements

`swapped(shown_a_first, shown_b_first)`: the judge sees (a, b), then (b, a). A side wins only if both orders prefer it; anything else is a tie. A judge that always prefers the first position therefore produces ties, never wins. `PairwiseTally` also counts the pairs whose orders disagreed: the judge's measured position bias, which the eval's calibration reports (ADR-0019).

The other two biases are handled where judging is set up, not here:
- judges are of another family than the writer: the army's `Escalation` keeps a judge in its family;
- drafts are compared at similar lengths.

## Selection

`select_lexicographic()` picks the candidate with the least first key, then the least second, and so on, and gives a full tie to the first. For drafts the keys are:
1. the ledger's deterministic violations;
2. the issues judges found;
3. minus the flow score (negate to prefer high).

A weighted sum would let a fluent draft that drops a must-quote beat a plain one that keeps it, and its weights would be one more thing to tune. A key that is not a number is worse than any number.

## Tiers

| | `fast` | `thorough` | `max` (default) |
|---|---|---|---|
| outline | chapters, else 1 outline; budget 8K | the same | chapters, else 3 outlines, boundary consensus; budget 16K |
| atomize | 1 sample, 1 repair | + a recall pass | 3 samples, voted (quorum 2 of 3); contested → the commander |
| plan | deterministic grouping | the commander assigns every atom; budget 8K | the same; budget 16K |
| write | 1 draft, 1 repair round | 2 drafts, judged; 2 rounds | 4 drafts, the top 2 judged; 3 rounds |
| seams | none | rewritten | rewritten, then checked again |

- **One deviation from ADR-0018's table.** The ADR's `thorough` outline cell omits the video's chapters, and `thorough` uses them as `fast` and `max` do. A video's chapters are its author's sections: no model's outline is truer. The decision is in `thorough.toml`'s header.
- **How sure a lone sample must be** (`[atomize] sure`, a probability in [0, 1]) is 0 in every shipped tier: a lone sample settles its sentence, as before P28. Runbook R6's sweep tries 0.9.
- **Read strictly** (`parse_tier()`). An unknown section or key, a value of the wrong type or out of range, `judge_top` of 1 (a judge compares at least two drafts), or waves that cannot draw their max are errors naming the tier, the section and the key. A key left out takes its default.
- **Canonical per stage.** `Tier::canonical(stage)` is the stage's settings as compact JSON, keys sorted, every value present. It enters the stage's cache key (plan P15), so changing one value re-runs exactly that stage and the stages after it, while writing out a default changes nothing.
- **Compute only rises** from `fast` to `max`, stage by stage, and a test checks it: a higher tier never buys less.
- **Variants, named by what they change** (`find_tier()`, P26): `max+write.thinking=true` is `max` with `[write] thinking = true`, and `thorough+write.drafts=3+plan.mode=group` changes two stages of `thorough`. Each change, `stage.key=value`, is set in the base's own file, its value read as TOML would read it (true or false, a whole number, a number, else a word), and the result is read by `parse_tier()`'s strict reader: a variant is refused exactly where a tier file would be, naming the variant whole. Its stages' canonical forms are its settings', so it shares every stage it leaves alone with its base. The eval sweeps variants against a baseline ([eval.md](eval.md#sweeps-and-the-report)).

## How it is tested

All of it is pure, so all of it is unit-tested, with exact expectations:
- `vote_test.cpp`: votes per item, the quorum as a share (two thirds is 0.66, and floating point is kept exact), ties, missing votes, the medoid among winners only, refused ballots, waves (more only where it helps, never past max, futility), wave checks; a lone sample settling only as sure as asked (a confidence not known is none, the bar reached at it and a hair under it), samples that agree settling however unsure each was, an unsure lone sample drawing a second, `sure` checked;
- `boundaries_test.cpp`: agreement within the tolerance, support, quorum, the earlier of an even split, order and duplicates in an outline, floating point;
- `pairwise_test.cpp`: the swap, the tally and its inconsistency count;
- `select_test.cpp`: lexicographic order, ties, NaN, refusals;
- `tier_test.cpp`:
  - the shipped tiers, and `max` as ADR-0018 describes it;
  - compute rising from `fast` to `max`;
  - the canonical form, exactly;
  - an empty tier is the defaults;
  - strict reading, error by error;
  - a variant: its base with each change set (true, a whole number, a number, a word), only the stage it changes keyed apart;
  - a variant read as strictly as a tier file: each malformed change, an unknown section, a key changed twice; every shipped tier writing out every section;
  - the compiled-in tiers are the source tree's;
  - `sure` read as a probability: 0 and 1 are ones, 1.5, −0.1, NaN and a word are not.

### How the tests were validated

Each rule was removed on purpose (69 mutants: 14 of them P26's for variants, 11 P28's for a lone sample's confidence and the tiers' `sure`), and the named test was confirmed to fail.

| Rule removed | Caught by |
|---|---|
| The quorum's slack for floating point; the quorum at all | `AQuorumIsAShareOfTheVotesCast` |
| A tie is not settled | `ATieIsNeverSettled` |
| Stray ballots, double votes, items listed twice are refused | `RefusesBallotsItCannotCount` |
| Counts are sorted, most votes first | `TheWinnerNeedNotBeVotedFirst` |
| The medoid among the winner's ballots only; a tie to the first | `TheMedoidIsTheWinnersMostTypicalText`, `PicksTheTextClosestToAllOthers` |
| Waves: futility, never past max, settled items left alone; their checks | `DrawMoreOnlyWhereItCanHelp`, `AreChecked` |
| Words lowercased; UTF-8 letters kept | `WordsAreLowercasedAndUnicodeKept` |
| Boundaries: spread, suppression, the quorum and its slack, the tolerance inclusive, the nearer neighbour on either side | `Boundaries.*` |
| Pairwise: the swap, its mapping, the inconsistency count, a tie's half, no pair's 0.5 | `Pairwise.*` |
| Selection: NaN the worst, an earlier key final, sizes checked, a tie to the first | `Select.*` |
| Tiers: unknown keys and sections, `judge_top` of one, waves, shares (range, NaN, whole numbers), the canonical form, defaults, modes, names | `Tier.*` |
| Variants: the base found, each change set, each kind of value as TOML reads it, every part of `stage.key=value`, an unknown stage, a key changed twice | `AVariantIsItsBaseWithEachChangeSet`, `AVariantIsReadAsStrictlyAsATierFile` |
| A lone sample settles only as sure as asked; a confidence not known is none; the bar's slack | `ALoneSampleSettlesItsItemOnlyWhenSure` |
| Samples that agree settle their item, their confidences aside | `SamplesThatAgreeSettleTheirItemHoweverUnsureEachWas` |
| A wave reads settledness from the vote's winner, not from the counts alone | `AnUnsureLoneSampleDrawsASecond` |
| `sure` is a probability: neither above 1 nor NaN; the tiers read it, 0 included, and key it | `Waves.AreChecked`, `Tier.IsReadStrictly`, `AStagesCanonicalFormIsItsCacheKeyMaterial` |

**What writing the mutants found, before any ran:** no test had a winner that was not voted first, so counts that were never sorted would have passed. That test exists now.

**What running them found:** four rules had tests that could not fail.
- **The medoid's restriction to the winner's ballots.** The fixture's losing text was unlike every winner's, so including it changed nothing. The loser now writes what one winner wrote.
- **That a later key never buys back an earlier one.** No fixture had an earlier key favouring the holder while a later key favoured the challenger.
- **A share's range, and NaN.** The test used atomize's quorum, which the waves check also guards. Outline's quorum, which has no second guard, is tested now.
- **The `thorough` tier's seams mode.** It was never asserted directly. Mapping "rewrite" to "recheck" would have kept compute rising from fast to max, and passed.

**P28's run** killed 12 of 13: its 11, and the 2 whose code moved (a wave reading settledness from the vote's winner, a share written as a whole number). One survived, a gap of the same kind as a share's: `sure` above 1 is refused twice, by the tier's reader and then by `waves_problems()`, in words that both end "a probability in [0, 1]", and the test read only those words. It reads the reader's own now (`'sure' must be a probability in [0, 1]`), and the final run kills it.

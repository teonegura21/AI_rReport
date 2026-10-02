# Reasoning: many answers, one decision

The models are local and fixed (ADR-0017). Asking a model once is the least it can do. With more compute it can do better, but only if the extra answers are combined well. `rr_reason` holds the combining rules: pure functions with no I/O, no model and no threads, so each rule is exact and unit-tested. The tiers that say how much each stage spends live here too ([ADR-0018](../adr/0018-compute-tiers-and-ceilings.md)).

## Pieces

| Where | What it is |
|---|---|
| `rr/reason/vote.hpp` | Votes per item (`vote_keyed()`); waves of samples only where they disagree (`Waves`, `next_wave()`); the medoid text |
| `rr/reason/boundaries.hpp` | Where several outlines agree a section starts |
| `rr/reason/pairwise.hpp` | A judge's preference, counted only if it survives swapping the order |
| `rr/reason/select.hpp` | The best candidate by criteria in order of importance |
| `rr/reason/tier.hpp` | The tiers: each stage's compute, strictly read, canonical for cache keys |
| `config/tiers/{fast,thorough,max}.toml` | The tiers themselves, compiled in |

## The evidence, and the rules it gives

| Finding | Rule |
|---|---|
| Self-consistency, majority voting over samples, beats a single answer (Wang et al., 2023), and beats LLM verifiers at equal compute (Singhi et al., 2025) | Samples are voted on, not judged |
| Allocating samples adaptively, where they disagree, is more than 4× as efficient as uniform best-of-N (Snell et al., 2024) | Waves: draw more only while an item is unsettled *and could still settle* |
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

At the `max` tier, atomize draws three samples at once, and what they still disagree on goes to the commander, thinking. The eval's sweep for runbook R6 (`eval/sweeps/r6.toml`) measures what the three samples and the commander's rulings each buy; whether further waves pay before escalation does is a variant to add to it.

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
- **Read strictly** (`parse_tier()`). An unknown section or key, a value of the wrong type or out of range, `judge_top` of 1 (a judge compares at least two drafts), or waves that cannot draw their max are errors naming the tier, the section and the key. A key left out takes its default.
- **Canonical per stage.** `Tier::canonical(stage)` is the stage's settings as compact JSON, keys sorted, every value present. It enters the stage's cache key (plan P15), so changing one value re-runs exactly that stage and the stages after it, while writing out a default changes nothing.
- **Compute only rises** from `fast` to `max`, stage by stage, and a test checks it: a higher tier never buys less.
- **Variants, named by what they change** (`find_tier()`, P26): `max+write.thinking=true` is `max` with `[write] thinking = true`, and `thorough+write.drafts=3+plan.mode=group` changes two stages of `thorough`. Each change, `stage.key=value`, is set in the base's own file, its value read as TOML would read it (true or false, a whole number, a number, else a word), and the result is read by `parse_tier()`'s strict reader: a variant is refused exactly where a tier file would be, naming the variant whole. Its stages' canonical forms are its settings', so it shares every stage it leaves alone with its base. The eval sweeps variants against a baseline ([eval.md](eval.md#sweeps-and-the-report)).

## How it is tested

All of it is pure, so all of it is unit-tested, with exact expectations:
- `vote_test.cpp`: votes per item, the quorum as a share (two thirds is 0.66, and floating point is kept exact), ties, missing votes, the medoid among winners only, refused ballots, waves (more only where it helps, never past max, futility), wave checks;
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
  - the compiled-in tiers are the source tree's.

### How the tests were validated

Each rule was removed on purpose (58 mutants, 14 of them P26's for variants), and the named test was confirmed to fail.

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

**What writing the mutants found, before any ran:** no test had a winner that was not voted first, so counts that were never sorted would have passed. That test exists now.

**What running them found:** four rules had tests that could not fail.
- **The medoid's restriction to the winner's ballots.** The fixture's losing text was unlike every winner's, so including it changed nothing. The loser now writes what one winner wrote.
- **That a later key never buys back an earlier one.** No fixture had an earlier key favouring the holder while a later key favoured the challenger.
- **A share's range, and NaN.** The test used atomize's quorum, which the waves check also guards. Outline's quorum, which has no second guard, is tested now.
- **The `thorough` tier's seams mode.** It was never asserted directly. Mapping "rewrite" to "recheck" would have kept compute rising from fast to max, and passed.

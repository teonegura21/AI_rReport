# Fidelity: the promises, checked

rReport promises an article faithful to its video: every sentence accounted for, every atom told, every number right, every quote verbatim. A model's word for any of that is worth nothing, because a model that drops a sentence does not know it dropped it. So everything code can check, code checks. `rr_fidelity` holds those checks. Each failure carries a message the repair mission will quote, and anything the repair cannot fix is published in the Fidelity Report, never hidden (ADR-0005).

## Two moments

| When | Function | Holds | Against |
|---|---|---|---|
| After atomize | `check_ledger(transcript, ledger, lexicon)` | Ledger A: the atoms | the transcript, or the window of it a mission was given; the video's lexicon |
| After write | `check_article(transcript, ledger, markdown, scope)` | Ledger B: the article, or a section of it | the atoms and the transcript |

Both return a `Report`: findings, and a `Coverage` that counts how much of each promise was kept. Reports add up (`Report::add()`), so the whole job's report is the sum of its windows and its article.

## The checks

Every check has a stable name, used by reports, repairs and the eval:

| Check | Severity | Holds true | Message, for example |
|---|---|---|---|
| `sentence-accounted` | error | every sentence became atoms, or a filler label | S6 is not accounted for: give it one or more atoms, or a filler label (disfluency, greeting, …) |
| `sentence-once` | error | never both | S2 is labelled repetition and told by A1: it is one or the other |
| `sentence-known` | error | atoms and fillers name only sentences given | A1 cites S9, which the transcript given has not: cite only the sentences you were given |
| `number-said` | error | an atom's numbers are in its sentences | A4 says "38%", which its sentences (S5) do not: they say "37%", "2023" |
| `quote-said` | error | a must-quote is its sentences' words, verbatim | A3's quote "the most wonderful element" is not what was said: its sentences say "the most beautiful element" |
| `name-said` | warning | an atom's names are in the transcript given | A2 names "Albert Einstein", whom the transcript given does not name: check it was said |
| `filler-empty` | error | a filler holds no number, name or lexicon term | S6 is labelled disfluency, yet holds the number "42": re-examine it, and give it atoms unless it carries nothing |
| `marker-valid` | error | markers are well formed | [a1] is no marker: write it [A1] |
| `marker-known` | error | markers name atoms of the ledger | [A9] names no atom: the atoms are A1 to A4 |
| `marker-scope` | error | and atoms the text tells: a section cites its own | [A3] is not among the atoms to tell here: cite only the atoms given |
| `atom-told` | error | every atom is cited | A4 is not retold: tell it, and cite it [A4] |
| `number-kept` | error | an atom's numbers reach the paragraph citing it | the paragraph citing A4 lacks "37%": write it, in digits or words |
| `name-kept` | error | so do its names | the paragraph citing A2 does not name "Marie Curie" |
| `term-kept` | error | and its lexicon terms | the paragraph citing A2 does not use the term "Nobel Prizes" |
| `must-quote-kept` | error | a must-quote stands, verbatim, where it is cited | A3 must be quoted verbatim, "the most beautiful element", and the paragraph citing it has nothing like it |
| `quote-verbatim` | error | every quotation is the transcript's words | the quote "radium the most lovely element" is not what was said: the transcript has "radium the most beautiful element" |
| `order-kept` | warning | within a section, atoms are told in the order said | A4 was said at S5, before what this section tells ahead of it: keep the order of the talk |

A filler may hide none of the video's words: the lexicon `filler-empty` holds it to is the atoms' own terms and the video's, its brief's (P17), which the atomize stage passes (P18). A window's atoms need not use a word the video uses elsewhere, and without the brief's lexicon a label would slip through there.

A finding also names the sentences and atoms it concerns, and where it is in the article, so a repair can be given exactly the evidence it needs. One problem gives one finding. A sentence both labelled filler and told by an atom is reported by `sentence-once` alone, not also by `filler-empty`.

## What texts are held to

Texts are compared as `rr_text` compares them ([text.md](text.md)):
- **Numbers by value.** "37%" is "thirty-seven percent", "2023" is "twenty twenty-three", and 37% is not 37 people. A bare "one" is not held: it is as often a pronoun ("one of them") as a count.
- **Names and terms folded,** so case and accents are aside. A name is kept whole or by its last word: "Curie" keeps "Marie Curie". This is the convention of prose, though it lets "Pierre Curie" pass for "Marie Curie" too. Distinguishing them is a judge's work (P24).
- **Quotes aligned word for word** (Smith–Waterman), punctuation, case and accents aside. An ellipsis (`…`, `...`, `[…]`) marks words left out: each part must be verbatim. A quotation of fewer than three words is a name, a title or a term, and is not held.
- **A quotation** is what stands between double quotes, curly or straight. A quote mark in code, fenced or `inline`, is code. A straight quote after a letter or a digit opens nothing: in `a 13" laptop` it is an inch mark. Code inside a quotation is read as its words, since quote marks promise the speaker's words whatever their typography.
- **The paragraph citing an atom** is every block with a sentence that cites it, read without markers. A number, name or term is kept if any of them holds it.
- **Closest words** are shown to a repair only when at least half the quote's words matched. Sharing a lone "the" with the transcript shows nothing, and the message says the quote is nowhere in the transcript.

## Duplicates: what telling the other would lose

Ledger B lets an atom go untold when it is declared a **duplicate** of one that is told (ADR-0005, rule 6): a speaker's summary of what they just said, a point made again for emphasis. The plan declares them (P19, [pipeline.md](pipeline.md#the-plan-stage)), and a duplicate declared wrongly is a promise broken silently: what only it said never reaches the article. So `duplicate_problems(atom, of)` says what telling only `of` would lose, and the plan stage sends each reason back to the planner:

| What `atom` has | Lost when | Message, for example |
|---|---|---|
| a number | `of` does not state it, by value | it says "1902", which A12 does not |
| a name | `of` neither gives it nor lists it, whole or by its last word | it names "Pierre", which A12 does not |
| a lexicon term | `of` neither uses nor lists it | it uses "half-life", which A12 does not |
| words to quote | `of` does not quote them, word for word | its words "the most precious thing" are to be quoted, and A12 does not quote them |
| a modality | `of`'s differs | it is hedged, and A12 asserted |
| a speaker | `of`'s differs, folded; a speaker unknown differs from one known | it is said by Pierre, and A12 by Ada |
| a source it reports | `of`'s differs | it reports the NRC, and A12 reports no one |

The rule is stricter than meaning, on purpose. Two atoms may mean the same and still differ here, and a duplicate refused costs a sentence of the article, while a duplicate wrongly accepted loses what was said. When in doubt, an atom is told. Kind, register and intensity are not compared: readers label them differently without either being wrong (the atomize stage does not vote on them either, [pipeline.md](pipeline.md#3-votes-and-what-they-do-not-settle)), and none is a fact a reader would miss.

## Scope: a section, or the whole

`check_article()` holds a text to an `ArticleScope`; by default, the whole article and every atom of the ledger. The write stage holds two kinds of text ([pipeline.md](pipeline.md#the-write-stage)):
- **A section's draft** is held to the section's atoms (`told`). It is not asked for the atoms other sections tell, and coverage counts its own. A marker of another section's atom is an error of its own, `marker-scope`: that atom would be told twice, or out of its place, and the writer was never given it, so whatever the sentence says of it is not from its evidence.
- **The article** is held to every atom, with the plan's declared duplicates (`duplicates`). A duplicate need not be cited. It counts as one atom of the article, told when it is cited or when the atom it repeats is. When neither is, `atom-told` names the atom it repeats, once: that is the atom to tell.

`marker-scope` is not `marker-known`: the atom exists, so the marker is not counted invalid, and an article cannot break it, since an article is held to every atom.

## Order: the fewest to move

A section's atoms should be told in the order they were said. A naive check, flagging every atom told after one said later, would flag a cascade: one atom pulled forward would make every atom after it look out of place. So the check finds a longest run of first citations whose positions in the talk never decrease (the longest non-decreasing subsequence, `O(n log n)`). The atoms outside that run are the fewest that must move, and only they are flagged. An atom's position is where it was first said, and only its first telling is placed: a recap that cites an atom again tells nothing out of order. Atoms first said in one sentence may be told in either order, which is why the run is non-decreasing rather than increasing. Headings start new sections, which may be ordered freely: an article may tell a talk's two halves in either order. Order is a warning, never an error. A writer may group related points, and M1's acceptance does not count it.

## Coverage: the acceptance measures

`Coverage` counts what M1's acceptance (runbook R3) and the eval's invariants (ADR-0019) measure:

| Measure | From | M1 target |
|---|---|---|
| sentences accounted | `sentences_accounted / sentences` | 100% |
| atom coverage | `atoms_told / atoms` | ≥ 97% |
| invalid markers | `markers_invalid` | 0 |
| must-quotes | `must_quotes_kept / must_quotes` | 100% |
| numbers | `numbers_kept / numbers` | ≥ 99% |

## Faults, caught

The oracle's fault injectors (P10, [testing.md](testing.md#the-oracle-and-fault-injection)) make the mistakes real models make. `tests/unit/fidelity/faults_test.cpp` makes each one that changes what a model writes on a faithful answer, and checks that one of these checks catches it:

| Fault | Caught by |
|---|---|
| `wrong_number` in the article | `number-kept` |
| `wrong_number` in an atom | `number-said` |
| `bad_marker` | `marker-known`, and `atom-told` for the atom it replaced |
| `dropped_reference` | refused as the ledger is read (an atom with no sentence), else `sentence-accounted` |
| a softened quote (`edit`) | `must-quote-kept` and `quote-verbatim` |
| a misattributed name (`edit`) | `name-kept` |

The remaining faults (refusal, truncation, broken JSON, running out while thinking, server errors) are caught before any text exists, by the army's checks and ladder ([agents.md](agents.md)).

## What waits

What needs a model (softening without a quote, misattribution in paraphrase, sentences that add) the write stage's judge reads, sentence by sentence and evidence first (P20b, [pipeline.md](pipeline.md#the-write-stage)): its readings choose between drafts and are published as readings, never as checks. The eval's own judges, frozen and calibrated, arrive with P24. Until then, a finding goes back to the mission that can fix it only from the stage that made the text: a window's Ledger A findings to its readers (P18), a section's Ledger B findings to its writer (P20).

## How the tests were validated

40 unit tests (`tests/unit/fidelity/`) work on one small talk. Each breaks one promise of a faithful ledger or article and expects exactly the finding for it, message included. They pass under ASan and UBSan. Each oracle fault (P10) is made on a faithful answer and caught, as the table above shows. The mutants (`tests/mutants/fidelity.toml`) each remove one rule and name the test that must fail without it.

1. **The first run killed 24 of 28.**
   - Four rules had no test, and each got one:
     - a bare "one": no test said "one";
     - an atom told twice: no test cited one twice;
     - atoms said together: no two atoms began at one sentence;
     - quote marks in code: the code test quoted nothing.
   - Three kills were muddy, because the suite crashed instead of failing:
     - two mutants left one finding where a test expected two. The test read the second finding after a non-fatal check of the count, so the suite crashed. That check is fatal now;
     - one mutant made the code read the first of no blocks, and crashed an earlier test. It now drops the finding only.

     `scripts/mutate.py` now names the test a suite died in.
2. **Writing the test for code found a false finding.** Quote marks inside inline code made a quotation, so `` `note("the finest metal of all")` `` was held against the transcript. The rule in place, skipping quotations that hold a backtick, missed it: that quotation sits inside the code and holds none. Now quote marks in code are code, read through rr_knowledge's `inline_code()`, made public for this. A quotation with code inside it is held by its words.
3. **Looking for similar cases found another.** In `a 55" screen and a 13" laptop`, the two inch marks made a quotation. A straight quote after a letter or a digit now opens nothing.
4. **One rule was redundant.** `ArticleView` kept a code block's text empty. No check could see it, since code blocks cite nothing and their text is never read, so it is gone.

The final run kills all 31 mutants, each by the test that claims it: 12 on Ledger A, 3 on what texts are held to, 16 on Ledger B. The two rr_knowledge mutants of the inline-code reader, rerun after the move, are still killed.

P19 added `duplicate_problems()`, with four tests: a duplicate that loses nothing is taken, numbers by value, a name by its last word, words and speakers folded; one that says more (a number, a name, a term) is not, unless the other lists it; words to quote must be the other's, word for word; and how it is said (a modality, a question, a speaker, a speaker unknown, a source) must be the other's too. Its 12 mutants are all killed by the tests that claim them.

P20 added the scope of a text (`ArticleScope`) and `Report::warnings()`, with two tests: a section held to its own atoms (coverage counts them, and a marker of another section's atom is `marker-scope`, not an invalid marker); and a duplicate excused, counted as told by the atom it repeats, with the one `atom-told` finding, of the atom to tell, when neither is cited. Its 11 mutants are all killed by the tests that claim them; one did not build at first (it left the scope unused, which `-Werror` refuses) and was written another way. A mutant of P13 had to be made exact: the line it changed now appears twice, once for the duplicates.

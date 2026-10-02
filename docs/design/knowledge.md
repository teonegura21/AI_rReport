# Knowledge: what the stages write

rReport turns a video into a knowledge model: the transcript's sentences, the atoms they became, the article that retells them. Every view (the article, the transcript, later the idea map and Q&A) is a projection of it. Each stage writes its part as one immutable, schema-checked JSON artifact, and the artifact is the truth (ADR-0004). `rr_knowledge` holds the types of those artifacts, their schemas, and the parser of the article's citation markers.

## Pieces

| Where | What it is |
|---|---|
| `rr/knowledge/ids.hpp` | The short IDs of an analysis, `S#`, `A#`, `V#`, each its own type |
| `rr/knowledge/transcript.hpp` | The transcript: sentences `S1…Sn`, when each was said, and by whom when known |
| `rr/knowledge/outline.hpp` | The outline: the video's sections, and the brief every later mission is given |
| `rr/knowledge/atoms.hpp` | Ledger A: the atoms every sentence became, or its filler label |
| `rr/knowledge/plan.hpp` | The plan: which paragraph of each section tells which atoms, and which atoms only another tells |
| `rr/knowledge/article.hpp` | The article's Markdown; its blocks, sentences and `[A#]` markers |
| `rr/knowledge/artifacts.hpp` | The artifact schemas, compiled in |
| `schemas/artifacts/{transcript,outline,atoms,plan,article}.json` | The schemas themselves |
| `tests/fuzz/markers_fuzz.cpp` | The marker parser's invariants, for libFuzzer |

## Short IDs

Inside an analysis, everything is named by a short, model-friendly ID (ADR-0009): sentences `S1, S2, …`, atoms `A1, A2, …`, on-screen items `V1, V2, …`. Models read them in packets, and write them in answers and markers. Code assigns every one and checks every one (ADR-0006). `SentenceId`, `AtomId` and `VisualId` are distinct types, so one cannot be passed for another.

The written form is strict: the letter, then a number from 1 without leading zeros, nine digits at most. "A12" parses; "a12", "A 12", "A012" and "A0" do not. A model's loose spelling is a finding to repair, never something to guess at. The marker parser below says what was meant.

## Artifacts

| Artifact | Written by | Holds |
|---|---|---|
| `transcript` | the transcript stage (P14) | sentences `S1…Sn` in the order said: text (NFC), `start_ms`, `end_ms`, speaker or `null` |
| `outline` | the outline stage (P17) | the sections, each from its `first` sentence to its `last`, with a title and a gist or `null`; the brief: the genre, the speakers (a name, a role or `null`), the lexicon (a term, a meaning or `null`), the video's gist or `null`; and where the sections came from: `chapters`, `models` or `pauses` |
| `atoms` | atomize (P18) | Ledger A: atoms `A1…An`, and the fillers |
| `plan` | the plan stage (P19) | each section by its `first` sentence: a heading, its paragraphs in the order told (a purpose or `null`, and the atoms each tells), and who planned it, `commander` or `grouping`; the duplicates: an atom, the atom that tells it, and why or `null` |
| `article` | write (P20) | the Markdown, with its markers; empty when the video says nothing to retell, every sentence filler |

Each schema follows the repo's rules for schemas (`rr/json/schema.hpp`): every object closed (`additionalProperties: false`), every field required, with `null` for what may be absent, and enumerations closed. Reading an artifact (`transcript_from_json()`, `outline_from_json()`, `atom_ledger_from_json()`, `plan_from_json()`, `article_from_json()`) validates it against its schema first. Then it checks what a schema cannot say:
- IDs run `S1, S2, …` and `A1, A2, …` in order, so a lookup by ID is an index;
- a sentence ends after it starts, and starts no earlier than the one before;
- sections run in order from S1, each starting right after the one before, and none ends before it starts;
- an atom's sentences are in order, once each;
- `quote` is present exactly when `must_quote` is;
- fillers follow their sentences' order, one each;
- a plan's sections follow the order said, each once; an atom is told once, or declared a duplicate, never both; a duplicate is not of itself, nor of an atom no paragraph tells, so duplicates never chain.

Every refusal names what is wrong ("A1 lists S2 after S3"), because these messages feed repairs.

What reading does **not** check is whether the parts agree with each other: whether the outline's sections end at the transcript's last sentence (`outline_problems()` says), whether the plan places every atom of the ledger in the section its first sentence is in (`plan_problems()` says), whether Ledger A accounts for every sentence of the transcript, whether every marker names an atom, whether every atom is cited. Those are the fidelity checks (P13), each with its own repair message.

### The outline

An article is written a section at a time, and each mission after the outline sees its own stretch of the video and the **brief**, no more (plan §3.7). The brief is what any mission needs to know of the whole: the genre, who speaks, the video's words for things, and its gist, about 1,500 tokens. Prompts put it in their system message, the part every mission of a stage shares, so an engine reads it once ([prompts.md](prompts.md#the-cache-aware-layout)).

Whatever is absent is `null`, never a guess: a section no model described has no gist; a speaker whose role the video does not say has none; a word the video does not explain has no meaning. Five genres name what changes how a video is retold: a `talk` (or a lecture), a `conversation` (a podcast, an interview, a panel), a `tutorial` (what is shown matters), a `narrative` (a documentary, a news report), and `other`. `sections_from` says what drew the sections, so a reader of the article knows how far to trust them: the creator's `chapters`, the commander's `models`, or `pauses` where neither was there ([pipeline.md](pipeline.md#the-outline-stage)).

### Atoms

An atom keeps what a retelling must not lose (ADR-0005):

| Field | Values | Why |
|---|---|---|
| `kind` | claim, fact, number, example, anecdote, aside, joke, hedge, opinion, question, instruction, definition, reference, visual | an article retells each kind differently, and loses none |
| `sentences` | one or more `S#` | where it was said: the timestamps come from these, never from a model |
| `speaker`, `attributed_to` | a name, or `null` | who says it, and whom it quotes or reports: misattribution is a finding |
| `modality` | asserted, hedged, hypothetical, reported, sarcastic, question | a hedge retold as a fact is a distortion |
| `register` | main, aside, joke, anecdote, example | an aside is not the thesis |
| `intensity` | low, medium, high | neither softened nor sharpened |
| `must_quote`, `quote` | a flag, and the exact words | strong, controversial, profane or precise statements stay verbatim |
| `names`, `terms` | lists | each must reach the paragraph that cites the atom |

`register` is a C++ keyword, so the member is `register_kind`. ADR-0005 names intensity without a scale. Three levels are what a check of softening and sharpening needs, and a finer scale would make models disagree without adding information. An atom's numbers are not a field: code reads them from its text (`rr/text/numbers.hpp`), as ADR-0006 asks for anything code can compute.

A sentence that holds no atom gets a filler label from the closed list: disfluency, greeting, repetition, sponsor, decorative. A filler that holds a number, a name or a lexicon term is re-examined, which is a fidelity check.

The atomize stage writes the ledger (P18, [pipeline.md](pipeline.md#the-atomize-stage)). Models answer a row a sentence, and an atom names the later sentences it also draws on (`also`); the stage turns the rows into this form: the atom cites its row's sentence and those, in order, once each, and is numbered in the order said. Its `must_quote` words become the `quote`, and the flag is set exactly when there are some.

### The plan

The plan says, before any prose, how the article retells the atoms (plan §5, stage 11): for each section of the outline, its heading and its paragraphs in the order told, each paragraph's atoms a closed list, and the atoms that only say again what another says. Ledger B then holds the article to it: every atom told, or declared a **duplicate** of one that is (ADR-0005, rule 6). Deciding this apart from the writing is what keeps it checkable: a writer handed a paragraph's atoms cannot drop one unseen, and code, not a model, says whether every atom has a place.

A plan section is the outline section with the same `first` sentence, and an atom is told in the section its first sentence is in: the article keeps the talk's order between sections, and within one an atom may move only where the planner says why (the order check is a warning, [fidelity.md](fidelity.md)). A section whose sentences are all filler has no paragraph. `planned_by` says who planned it: the `commander`, or `grouping`, the atoms told in the order said, a paragraph at a time, where no model planned it (the `fast` tier, or a section no answer planned, [pipeline.md](pipeline.md#the-plan-stage)). A grouped paragraph has no purpose, since no model said what a reader learns from it.

A duplicate names the atom that tells it, which a paragraph must tell, so duplicates never chain. Whether an atom may be one at all is a fidelity rule (`duplicate_problems()`): only when telling the other loses nothing it says, nor how it says it.

### What waits for its stage

On-screen items (`V#`) arrive with frames (M4). Each schema is defined when the stage that writes it is, so no shape is guessed early. A schema's text enters the stage cache key ([pipeline.md](pipeline.md#the-cache-key)), so changing one invalidates exactly the artifacts written under it. The pipeline's first artifact, the `source` (`schemas/artifacts/source.json`), is registered here with the others, and so is the eval's `silver` (`schemas/artifacts/silver.json`: a video's annotation drafted by models, [eval.md](eval.md#silver-drafted-by-models)).

## The article's markers

The writer composes Markdown with inline markers (ADR-0006): `He hedges: "I could be wrong" [A42].` Markers trace every sentence of the article to the atoms it retells, and so to the seconds of video they came from. They are read by code, exactly.

**A marker** is `[A`, a number from 1 without leading zeros, and `]`: `[A41]`. A sentence that retells several atoms writes several markers: `[A41][A43]`.

**A bad marker** is a citation that is no marker. It is reported with what to write instead, and the repair message quotes that:

| Written | Reason given |
|---|---|
| `[A41, A43]`, `[A41 and A43]` | one atom per marker: `[A41][A43]` |
| `[A41-A43]` | one atom per marker, each atom of a range too: `[A41]…[A43]` |
| `[a41]`, `[ A41 ]`, `[A041]`, `[A 41]` | write it `[A41]` |
| `[S12]`, `[V3]` | the article cites atoms, not sentences or frames |
| `[A0]` | atoms are numbered from A1 |
| `[A]`, `[A#]` | the atom's number is missing |
| `[A41 above]` | a marker holds an atom's ID only |
| `[A1234567890]` | no atom has that number |
| `[A41` (unclosed) | close it with `]` |

A bracket is a citation when what it holds starts with A, S or V and a digit, with spaces allowed between. `[Applause]`, `[1]`, `[see above]` and a footnote's `[a]` are text. Bad markers are never guessed into markers: an article that cites `[a41]` is repaired, not silently trusted.

**Code holds no markers.** In fenced code and in `inline code`, a bracket is code, and `\[` is a bracket written as text.

**A marker follows what it cites.** One written after its sentence's terminator still belongs to that sentence: in "Water boils. [A12] Ice melts.", A12 is cited by "Water boils.". The sentence splitter would start a new sentence at the bracket. So markers at the start of a sentence, before any of its words, move to the sentence before, and a sentence of markers alone joins it. At the start of a block, there is no sentence before, and they stay.

**Blocks** are read as Markdown is: headings, paragraphs, list items, block quotes, and fenced code, which runs to the end if never closed. Blank lines separate blocks, and Windows line ends are lines too. Every block but code is cut into sentences by `rr/text/sentences.hpp`. A block quote's sentence may therefore hold the `>` of its next line, which checks ignore since it is neither a word nor a number.

`without_markers()` gives a span's text as a reader sees it: "He hedges [A42]." gives "He hedges.". Checks compare that text, so "A41" is never read as a name, and bad markers stay visible until repaired.

`inline_code()` gives the inline code of a span as CommonMark reads it: from a run of backticks to the next run of as many, a run never matched being text. The parser reads no marker inside, and the fidelity checks read no quote mark inside ([fidelity.md](fidelity.md)).

## How the tests were validated

43 unit tests state the rules, and the parser is fuzzed like `rr_text` ([testing.md](testing.md#fuzzing)): every push replays its corpus, and the `fuzz` job searches. Five minutes of libFuzzer, 1,580,114 inputs, broke no invariant. The steps before that each found something:

1. **Reviewing the parser before its first build** found three defects:
   - `[A012]` was silently text. It passed the "written exactly" comparison, then failed the strict ID parse;
   - a footnote's `[a]` was reported as a missing atom number;
   - "One. [A12] Two." gave A12 to "Two.".
2. **Running the mutants** (`tests/mutants/knowledge.toml`) killed 33 of 43 at once. Of the other ten:
   - six were rules no test checked, and each got its test:
     - a single backtick inside a double-backtick code span;
     - a first sentence of markers alone before a real one;
     - `#5` without a space is no heading;
     - a four-backtick fence is not closed by three;
     - a CRLF blank line separates paragraphs. The CRLF test passed even with carriage returns kept;
     - finding an atom in a ledger built out of order;
   - one was equivalent. The parser tested for an uppercase A that "written exactly as `A<n>`" already implies, and that test is gone;
   - two did not compile (GCC's `-Wlogical-op`, an unused constant) and were rewritten;
   - one was killed by the ID constructor's assertion rather than the test named.

The final run kills all 43 mutants: 3 on IDs, 30 on the parser's citations, sentences, blocks and reading, 4 on the transcript, 6 on the ledger.

P13 made the inline-code reader public, as `inline_code()`, with a test of its own. The two mutants on it, rerun after the move, are still killed.

P17 added the outline (`rr/knowledge/outline.hpp`). Six tests state its rules: it reads back what it wrote, every optional field both ways; it names its genres and where its sections came from; sections run from S1, each right after the one before; none ends before it starts; the schema refuses an unknown field, an empty title or gist, no sections, a sentence that is not one, a genre or a source of no kind; and `outline_problems()` says where an outline and its transcript disagree. Its seven mutants are all killed by the tests that claim them.

P19 added the plan (`rr/knowledge/plan.hpp`). Eight tests state its rules: it reads back what it wrote, a purpose and a reason both ways; it names who planned a section; a section may have no paragraph; sections run in the order said, each once; an atom is told once or declared a duplicate, never both, and a duplicate is neither its own nor of an atom no paragraph tells, so duplicates never chain; the schema refuses an unknown field, an empty paragraph, purpose or heading, an atom written loosely, an unknown planner, no duplicates; and `plan_problems()` says where a plan, its ledger and its outline disagree. Its 18 mutants are all killed by the tests that claim them.

# Prompts: how rReport talks to its models

Prompts are the product's most important source code. A change of one sentence can lift or sink every article, and nothing in a compiler or a type checker will notice. So prompts get what source code gets: versions, reviews, tests that run on every push, and numbers from the evaluation before a change is kept. This note says how they are written, rendered and checked (`rr_prompt`), and why each rule exists.

## Pieces

| Where | What it is |
|---|---|
| `prompts/manifest.toml` | Every prompt's entry: purpose, answer schema, shared variables, known failure modes, and its numbered versions |
| `prompts/<name>/system.md.inja` | The system message: the role, the rules, what every order of a stage shares |
| `prompts/<name>/task.md.inja` | The last user message: the order's packet and its task |
| `prompts/<name>/example.json` | The data a prompt is rendered with, as an example: `{"shared": {…}, "orders": [two or more]}` |
| `schemas/missions/*.json` | The answers' schemas (the portable subset, [llm.md](llm.md)) |
| `src/prompt/` (`rr_prompt`) | Loading, rendering, the checks, token estimates |

Templates use [inja](https://github.com/pantor/inja) (v3.5.0, fetched at its tag, ADR-0012), a C++ implementation of Jinja's syntax: `{{ variable }}`, `{% for x in list %}`, `{% if … %}`.

## The cache-aware layout

**The rule.** Everything the orders of a stage share goes in the **system** message. Only the order's own packet and task go in the **last user** message.

**Why.** An engine keeps what it has read in its slots' attention cache. A new request whose prompt starts like an earlier one skips reading the common part. On a hybrid model like the commander (Qwen3.8, partly recurrent), that reuse happens only from checkpoints, the latest at the start of the last user message. A stage sends dozens of orders with the same role, rules and video brief: put those first, identical, and each order after the first reads only its packet. `rreport bench --cache-check` measures it on the pod (runbook R1); the nightly live tests check it against a real llama-server.

**How it is enforced**, so it holds without anyone remembering it:
1. **Statically.** The manifest lists, per prompt, the variables its system part may use (`shared`). The checks walk the system template's syntax tree and refuse any other variable. Walking the tree, not rendering an example, checks branches the example never takes too.
2. **By construction.** Rendering gives the system template the shared data and nothing else. A slip can only fail loudly; it can never leak an order's data into the shared prefix. So the system message is the same for every order of a stage.

## Strict rendering

A variable the data does not define is an **error**, not an empty string. A prompt missing its packet would otherwise ask the model about nothing, and the model would answer anyway, plausibly. A key in both the shared data and the order's is also an error, because which one is meant is ambiguous.

## Versions

Every prompt keeps its history in the manifest: one `[[prompt.version]]` per change, numbered 1, 2, 3…, each with the content hash of that version's files (both templates, and the answer schema, which is also what the model is told). The checks require the current version's hash to be the files' hash. A changed prompt therefore fails until it gets a new version, and the failure says exactly what to add:

```
prompt 'atomize' changed since version 3: add [[prompt.version]] number = 4, hash = "9f2c…", with a note on what changed
```

Why numbered versions, when the cache keys already use the content hash: people need names. The evaluation reports "atomize@4 against atomize@3", a finding names the prompt that produced it, and the history says what each version changed and why.

## Token estimates

A mission must fit the slot that serves it, with room for its answer (the engine reports `ctx_per_slot`, [runtime.md](runtime.md#context-per-slot-and-the-shared-pool)). A prompt that does not fit is refused, or, in a shared pool, fails every request in flight. So packets are sized before they are sent, and one that would not fit is split deterministically, never cut short in silence.

Exact counts need the model's tokenizer, which lives on the pod. An estimate is enough if it errs high: tokens per character, calibrated from calls already made (each call's record has its prompt and the tokens the engine counted). `calibrate()` takes the 90th percentile of the observed ratios, so nine prompts in ten are at most as long as estimated, and the rest by little. Each message adds a few tokens of template markers. Before any calibration, three characters a token: English usually runs four, so the default overestimates.

## Writing a prompt

- **Reason, then format.** Answer schemas put evidence and rationale fields **before** verdicts. A constrained decoder writes fields in schema order, so the model quotes before it decides ([llm.md](llm.md)).
- **Exemplars are synthetic.** Examples in prompts are written for the prompt, never taken from evaluation videos: an exemplar from the test set would make the evaluation measure memory.
- **Say how it fails.** `failure_modes` lists what reviewers have seen go wrong, so the next change is checked against it.
- **Put a block tag on a line of its own.** inja's `trim_blocks` removes the newline after a `{% … %}` tag, and the spaces before it too: `.{% if x %} Its` renders as `.Its`. The first prompt shipped with that slip until its test read the rendered text.

The first prompt is `punctuate` (P15): it puts the punctuation back into a window of automatic captions, and the transcript stage lays it onto the captions' words without taking a word from it ([pipeline.md](pipeline.md#the-prompt)). The eval's `annotate` and `annotate_more` draft silver (P16, [eval.md](eval.md#silver-drafted-by-models)). The outline stage asks three (P17, [pipeline.md](pipeline.md#the-outline-stage)): `outline`, a judgment, thinks; `describe` and `brief`, which read and sum up, do not. The atomize stage asks one, `atomize` (P18, [pipeline.md](pipeline.md#the-atomize-stage)), in three **modes** that share its system part, rules, example and brief, and differ only in the task: `read` a window; `settle` the sentences readers disagreed on, shown what each made of them; `recall` what a sentence's atoms miss, shown its atoms. One prompt, not three, because the rules are the same whatever the mode, and a rule that drifted between three copies would make the commander's settling judge by another standard than the infantry read by. Its answer puts each atom's evidence (the sentences it also draws on, its names, its terms) and its words before its labels. The plan stage asks `plan` (P19, [pipeline.md](pipeline.md#the-plan-stage)), thinking, a section at a time: its shared part shows the article's sections, so each section is planned knowing the others, and its answer gives each paragraph's purpose before its atoms and each duplicate's reason before its atoms, then the heading last, once the section is planned. The write stage asks `write` (P20, [pipeline.md](pipeline.md#the-write-stage)), without thinking, a section at a time. It is the first prompt answered in **prose**: it has no schema, because an article is Markdown, and a decoder held to a schema could only wrap the prose in JSON. Code checks the prose instead, sentence by sentence, as Ledger B checks an article ([fidelity.md](fidelity.md)). Its shared part shows the article's title, gist, speakers, lexicon and sections. Its task gives the section's paragraphs, each with its purpose and its atoms in full: an atom's words, when it was said, its labels (the kind, then a modality, register or intensity other than the usual), who said it and whom it reports, its words to quote, its names and its terms. The example in its system part shows a packet exactly as the stage writes one, so the model is never shown one shape and given another. Where the tier fuses (P29), it asks `fuse` of the commander too: the drafts of a section that passed, best first, with the paragraphs as their writer was shown them, for the one draft that takes the best of each (Self-MoA). Answered in prose and held to the writer's checks, its system part has the writer's rules and three of its own: what a draft adds to an atom is not written, each atom is told once, and no draft is better for coming first or for being longer. Its example shows three drafts, one of which adds what no atom says, and the answer taking nothing of it. It asks two more of the infantry, of another family than the writer: `judge` reads a draft's sentences against their atoms, the evidence quoted before each verdict, and `flow` says which of two drafts reads better, its reasons first, asked in both orders. The seams stage asks `seam` (P20c, [pipeline.md](pipeline.md#the-seams-stage)) of the commander, without thinking: the two paragraphs where sections meet, rewritten together, each shown with the atoms it cites. Answered in prose like `write`, and checked by the same code. Its system part shows only the video's title and channel, so every seam of an article shares it. The seams stage asks `judge` too, of a seam as written and as rewritten: its second version names what it checks in the task (`Check the seam between section 1, "…", and section 2, "…", sentence by sentence.`), where the first could only say "the section".

## The eval's judges

Three prompts read for the evaluation, never for the pipeline (P24, [eval.md](eval.md#the-judges)), and a frozen panel (`eval/judges/eval-v1.toml`) pins each by version and hash: a change to one of them is a new panel, so the evaluation's numbers stay comparable while the pipeline's prompts improve.
- **`eval_fact`** says whether an article tells one fact of its gold, shown the fact (its claim, and the words the video says it in) and the article's sentences most likely to tell it: told, partly, or not told, the sentences' words quoted first. Its two examples are a fact told in other words, and one told without its number: partly.
- **`eval_support`** reads a paragraph's sentences against **the video's own sentences**, not the atoms the pipeline's `judge` reads: an atom that misread the video passes the pipeline's judge and fails this one. Each of the video's sentences is shown as `S12 [1:02, Ada] The words.`, who says it inside the brackets, so that quoting the words never quotes the label. Its issues are the pipeline's scale, so the two read alike where they agree.
- **`eval_flow`** says which of two versions of a section a reader who has not seen the video would rather read, its reasons first, asked in both orders (P25). Its words are its own, not the pipeline's `flow`, which the writer's drafts are chosen by, and its example prefers the **longer** version, which explains a term the shorter one leaves bare: judges favour length, and an example that rewarded the shorter would teach the opposite slant instead of neither.

Each has a schema of its own, which names its prompt: the oracle, and the fake engine of the end-to-end tests, tell the three apart by it, as they could not tell `annotate` from `annotate_more`.

## How it is tested

| Level | What |
|---|---|
| L0, every push | `TheShippedPromptsHaveNoProblems`: the prompts compiled into the binary have no problem, and are the source tree's |
| Unit | `library_test.cpp`: the manifest, strict rendering, the system message the same for every order, shared variables only (in untaken branches, else branches and loops too), versions and hashes, schemas, examples, orphan directories, nothing read from the filesystem. `tokens_test.cpp`: rounding, fitting, calibration's quantile |

### How the tests were validated

Each rule was removed on purpose (25 mutants), and the named test was confirmed to fail.

| Rule removed | Caught by |
|---|---|
| The system template's variables are collected | `TheSystemPartMayUseOnlySharedVariables` |
| …in a loop's source, an `if`'s branch, an `else` | `TheSystemPartMayUseOnlySharedVariables` |
| …but not a loop's own variables, nor inja's `loop` | `ReadsTheManifest` |
| Only `shared` variables pass | `TheSystemPartMayUseOnlySharedVariables` |
| The system part is rendered with the shared data alone | `RenderingIsStrict` |
| A key both shared and the order's is refused | `RenderingIsStrict` |
| Templates read no files | `TemplatesReadNothingFromTheFilesystem` |
| The schema is part of the hash | `ASchemaIsPartOfThePrompt` |
| Each file is framed in the hash (text moved between the parts changes it) | `AChangedPromptNeedsANewVersion` |
| Versions count from 1; no hash repeats | `VersionsCountFromOneAndNeverRepeatAHash` |
| A change needs a new version | `AChangedPromptNeedsANewVersion` |
| Orphan directories, missing examples, examples that do not render | `EveryPromptHasItsFilesAndAnExampleThatRenders` |
| A schema must compile | `ASchemaIsPartOfThePrompt` |
| Estimates round up, and count each message's markers | `CountsCharactersAndRoundsUp` |
| A prompt and its answer may fill the context exactly | `APromptFitsWithItsAnswerOrNot` |
| A quantile below the median, or not a number, is refused | `CalibrationNeedsSomethingToGoOn` |
| Records without characters or tokens are left out | `CalibrationTakesAHighQuantileOfWhatWasSeen` |
| The quantile's rank rounds up | `CalibrationTakesAHighQuantileOfWhatWasSeen` |

**What writing the mutants found, before any ran:** four rules had no test that could fail. The system part's isolation from the order's data was only implied, else branches were never tried, the hash's framing was invisible (the tests computed hashes the same way the code did), and calibration's rounding and its exclusions happened to give the same answers either way. Each has its test now.

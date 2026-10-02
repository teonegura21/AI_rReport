# The evaluation's data: sets, snapshots, gold, and silver drafted by models

M2 measures rReport against what its videos say (ADR-0019). That needs three things before any measuring:
1. **a fixed set of videos**, each with the right to use it;
2. **their captions, frozen**, so that a change on YouTube changes nothing in the eval;
3. **what each video says**, written down once by a person: its gold annotation.

P16 builds that data, and the tools that make it cheap to produce. Writing down everything a lecture says takes a person most of a day; correcting a good draft takes an hour or two. So models draft it, as **silver**, and a person corrects it into **gold** with a review sheet beside it (runbook R2). P23 takes the set's videos through the article stages and scores each article by code ([Runs and scores](#runs-and-scores)); judges that read what code cannot are P24.

| Piece | Where | What |
|---|---|---|
| The set | `rr/eval/set.hpp`, `eval/sets/<set>.toml` | The videos, their licence and attribution, type, caption kind, split, pinned snapshot |
| Snapshots | `rr/eval/snapshot.hpp`, `eval/data/<id>/` | Each video's source artifact, read once through the pipeline's source stage, pinned by hash |
| Gold | `rr/eval/gold.hpp`, `eval/gold/<id>.toml` | Facts, must-quotes and terms, anchored to the timeline; silver until a person reviews it |
| Silver | `rr/eval/annotate.hpp` | The `annotate` stage: the commander's three drafts per window, joined, then the other family's additions |
| The review sheet | `rr/eval/sheet.hpp` | Markdown: each fact beside a link to its moment, how many drafts found it, whether its quote is said there |
| Runs | `rr/eval/run.hpp`, `migrations/0006_eval.sql` | A label, a set, a split, a tier and replicates; which article job is which video and replicate; the scores kept |
| Scores | `rr/eval/score.hpp` | Each article measured by code: the Fidelity Report's figures, gold's quotes, terms and facts cited, its length, what its job cost |
| Export | `rr/eval/export.hpp` | A run's scores, a row per video and replicate, as CSV or JSON |
| The files' reader | `src/eval/toml_io.hpp` | Strict TOML: every key asked for by name and type, any other key an error naming where it is |
| Prompts and schemas | `prompts/annotate`, `prompts/annotate_more`, `schemas/missions/annotate.json`, `schemas/artifacts/silver.json` | What the models are asked, and the forms of their answers and of silver |

## The set

```toml
[[video]]
id = "fed-talk-1"                # [a-z0-9-], 1 to 64, not starting with '-': it names a directory
title = "Monetary policy, explained"
url = "https://www.youtube.com/watch?v=abc123XYZ_0"
type = "talk"                    # talk | conversation | tutorial | narrative
captions = "manual"              # manual | automatic
split = "dev"                    # dev | test (ADR-0019)
licence = "us-public-domain"     # cc-by | us-public-domain
attribution = "Board of Governors of the Federal Reserve System"
snapshot = "1acc46b4…"           # optional: the snapshot's hash, pinned
```

**Read strictly.** A set is written by hand, and a slip that was silently ignored would measure something else than its author meant: a video without its split, captions of the wrong kind averaged in. So every key is asked for by name and type (`detail::Table`). A missing key, an empty text, a value out of its choices, a key nobody asked for, two videos with one id, or broken TOML is an error that names the file, the video's number and the key: `eval/sets/m2.toml, video 2: unknown key 'lisence'`.

**Only CC BY or the US public domain.** The eval commits its snapshots and may publish what it makes of them, so it needs the right. Non-commercial and no-derivatives licences are refused by the reader itself: `licence` has two choices.

## Snapshots

`rreport eval fetch` reads each video **once**, through the pipeline's own source stage (yt-dlp, the captions in the language spoken: [pipeline.md](pipeline.md#the-source-stage)), and keeps what it read in `eval/data/<id>/`:
- `<id>.source.json`: the source artifact itself, written readably (indented);
- `snapshot.toml`: its hash, when it was read (to the second, UTC), and from where.

From then on the eval reads the snapshot, never YouTube.

- **The snapshot is the artifact.** Its hash is the BLAKE3 of the artifact's canonical JSON, which is also its hash in a library. `rreport ingest eval/data/<id>/<id>.source.json` reads it as a saved source (`*.source.json`: [pipeline.md](pipeline.md#the-source-stage)), so a snapshot runs through the same stages, with the same keys, as the video would. A test runs the source stage on a snapshot and gets the snapshot's hash back.
- **Pinned by hash.** `snapshot.toml` keeps the hash, so a snapshot edited by hand, or damaged, is refused (`Errc::integrity`: "fetch it again"). The set may pin the hash too; a snapshot fetched again that differs from the pin is refused (`Errc::conflict`) until the set says otherwise. `eval fetch` prints the line to pin.
- **Checked before it is kept.** The captions must be of the kind the set says: a set that expects manual captions and gets automatic ones is a mistake to stop at, not a result to average in. A video the set calls CC BY must carry YouTube's Creative Commons line: only a licence the page states can be relied on. US federal works carry no such line, and need none.
- **Read once, unless asked.** A snapshot kept is never read again; `eval fetch --again` reads it again, on purpose, and the pin decides whether the new one may replace it.

## Gold

```toml
video = "radium-talk"
snapshot = "1acc46b4…"          # the snapshot it was annotated on
status = "gold"                 # silver: drafted, unreviewed; gold: reviewed
reviewer = "Ada"                # required for gold
drafted_by = "annotate@1 ×3 on qwen3.8-27b-q8; annotate_more@1 on gemma-4-26b-a4b"

[[fact]]
id = "F2"                       # kept when facts are corrected
start = "0:06"                  # as a player shows it: "3:04", "3:04.25", "1:02:03.5"
end = "0:10"
importance = "core"             # core: an article without it misses the point; detail
found = 3                       # how many of the drafts found it
quote = "Marie Curie measured 7 samples in 1902."
claim = "Marie Curie measured seven samples in 1902."

[[must_quote]]
start = "0:09"
end = "0:15"
quote = "the proof load"

[[term]]
term = "proof load"
meaning = "the most a bridge carries without bending for good"
```

**Anchored to the timeline (ADR-0009).** A transcript's sentence numbers change whenever its segmentation improves, so gold never names an `S#`. A fact holds the stretch of the video that says it and a quote of the words that say it, and is found again in any transcript by both: the quote, said verbatim (`rr/text/align.hpp`: case, accents and punctuation aside) in the sentences said within the stretch, give or take two seconds (`said_there()`, `kAnchorSlack`). The slack is there because captions' times are approximate, and a person reads them off a player.

**Times as a player shows them.** ADR-0019 writes the anchor as `(t0_ms, t1_ms, quote)`. The file spells it `start` and `end`, as players show times, because people read and correct it against a player: "3:04.25" is checked at a glance, 184250 is not. The reader also takes plain seconds ("184") and hours ("1:02:03"); the writer drops trailing zeros. The file reads back equal to what was written, quotes, backslashes, new lines and control characters included.

**Silver until reviewed.** Models write `status = "silver"`; a person who corrected the file sets `status = "gold"` and names themself as `reviewer`. The eval scores only gold. A gold file without a reviewer is a problem the sheet reports.

**What is wrong is said as what to correct** (`gold_problems()`): a fact or must-quote that ends before it starts; a quote not said in its stretch ("F7: "We call it the proof…" is not said between 0:09 and 0:15: correct its quote, or its times"); gold without a reviewer. A long quote is shown cut at 60 bytes, never inside a character.

## Silver, drafted by models

The `annotate` stage runs after `source` and `transcript`, as a pipeline stage like any other: keyed, cached, accounted and resumable. An annotation interrupted halfway asks again none of the calls it made (ADR-0020).

1. **Windows.** The transcript is cut into windows of whole sentences, each sent with a few sentences of context on either side: up to three, as many of them as fit 1,000 bytes of lines. The context lets a model read the window's first and last sentences as they were said; only the window's own sentences may be cited.
2. **The commander drafts each window three times, thinking** (`prompts/annotate`). The three drafts are three samples of one reading of the prompt (`n = 3`, plan P3), with seeds the job's salt derives: a replicate draws new samples. Every item cites the sentences that say it and quotes them; a term must be said in the window. A draft that breaks these rules is sent back with its problems, at most eight at a time, said as what to do instead ("fact 3: "Pierre Curie measured 7 samples" is not what S5 says, word for word: copy the quote from it"), and climbs the escalation ladder (rr/agents/ladder.hpp).
3. **The drafts are joined.** Two facts are one when they cite a sentence in common and one's quote holds the other's. The fact is kept once, with the first draft's words, on every sentence any draft cited, and `found` counts the drafts that found it (a draft listing a fact twice counts once). A fact most drafts call detail is detail; otherwise core, ties included: an account that keeps too much is corrected faster than one that misses the point. Must-quotes are joined the same way; terms by their folded form, a meaning filling an empty one and never replacing one.
4. **A model of the other family reads each stretch again** (`prompts/annotate_more`), on the infantry, with the facts found in that stretch beside the sentences ("- [S12, S13] claim"), and adds what they miss with `found = 0`. Two families see differently (ADR-0017). Its additions are joined in by the same rule, so a fact it repeats in other words is dropped only when its quote is the same.
5. **Silver** is the artifact (`schemas/artifacts/silver.json`): facts with their sentences, times, quotes, claims, importance and `found`; must-quotes; terms; who drafted it (`annotate@1 ×3 on <commander>; annotate_more@1 on <infantry>`). Once the job is done, `run_annotation()` writes it as the video's gold file, status silver, facts numbered F1, F2, … in the order said, times and quotes only.

**Windows fit their engines exactly.** A window is filled with whole sentences up to a size its engine suggests (`window_sizes()`):
- the commander's up to 12,000 bytes of lines: after its thinking budget, a third of the room for the window and two thirds for an answer of up to twice what it reads (a dense passage gives a fact a sentence, each with a quote and a claim of its own, in JSON);
- the other family's up to 3,000 bytes: after its answer (1,024 tokens), half the room for the window and half for the facts found in it.

Then its mission is built and, while the engine would not hold it (`agents::Army::holds()`: the very test the squad applies), it gives up sentences from its end, to the next window: the pipeline's windows, shared since with every stage that reads a transcript ([pipeline.md](pipeline.md#what-every-stage-that-asks-models-shares)), which find the largest window the engine holds by bisection. So no mission is ever refused for its size, whatever the title, the context or the facts found, and a sentence too long for its engine even alone is a finding ("the sentence S3 (0:06 to 0:40) has no draft: even alone, with its context and an answer, it is more than qwen-commander holds at once"). On a 48 GB pod (the commander 32,768 tokens a sequence, the infantry 4,096 a slot) the windows are 12,000 and about 2,800 bytes. A test with a commander holding 3,800 tokens shows windows giving up sentences, every sentence drafted exactly once, and each answer's budget twice what it reads.

**The stage's key** holds the prompts' versions and content hashes, the army's fingerprint (its models and how they sample, the thinking budget included), the window sizes **and what each engine holds**: how the transcript is cut depends on the engines' sizes, which `army_fingerprint()` leaves out (they change how fast, not what, for every other stage). The stage samples, so the job's salt is in its key too.

**Findings.** A window no draft passed the checks for, the models declined, or a job ceiling stopped is a finding naming its sentences and minutes, pass by pass, in the order said. A ceiling makes the result this job's alone (`reusable = false`): a job allowed more would have drafted more.

### Jobs that annotate

An annotation is a job of the library, of its own kind (`library::JobKind::annotation`, `migrations/0005_job_kind.sql`). Its source is the snapshot, never YouTube.

- `rreport eval annotate` takes up an unfinished annotation job of the same snapshot (running, stopped, needing the user) rather than starting another: its memo answers every call it made. A failed job is not taken up: what failed would fail again.
- `rreport resume` refuses an annotation job: it runs the article's stages, and would mark the job done without its annotation. `rreport jobs` shows each job's kind.
- A gold file is never replaced by a draft once reviewed, and a silver one only with `--replace`. A gold file that cannot be read is never replaced either: it may be a person's work half done.

## The review sheet

`rreport eval sheet ID` prints Markdown that a person keeps open beside the gold file:
- each fact: its stretch as a link to that moment of the video (`&t=184s`), how many drafts found it, its importance, claim and quote, and whether the quote is said there (✓ or ✗);
- a fact only one draft found is marked `?`, the first to doubt;
- must-quotes and terms;
- what to correct: `gold_problems()`, and, first, gold drafted on another snapshot than the one kept now (YouTube's captions changed, and every anchor must be checked again).

**Checked without a model.** Quotes are aligned without their punctuation, so the sheet checks them against the snapshot's captions cut into sentences as they are (`caption_transcript()`): reviewing never needs the pod.

## Runs and scores

A number about articles means something only next to how they were made. A **run** fixes that: one set, one split, one tier, a number of replicates, under a label that compare and gate (P25) name it by (`rr/eval/run.hpp`, `migrations/0006_eval.sql`).

**`rreport eval run --label L`** takes each video of the split through the article stages, once a replicate:
- its jobs are ordinary article jobs, made from the video's snapshot: they share the stage cache, the call log and the battle log; `rreport jobs` lists them, and `rreport report` pages them;
- a replicate's number is its job's salt, so the replicates draw different samples (ADR-0020's seeds derive from it), while a run made again draws the same;
- replicate by replicate: a run interrupted early has every video once before any twice;
- a job is made when its turn comes, never ahead, so an interrupted run leaves no job that never started;
- the same label again takes the run up: a job done is kept, one not done is resumed (its memo answers every call it made), one missing is made. A label names one run: asked of another set, split, tier or number of replicates, it is refused.

**`rreport eval score --label L`** scores each job done by code alone (`rr/eval/score.hpp`). The scores are kept in the library, and replaced when the job is scored again:

| Metric | What |
|---|---|
| `sentences_accounted`, `atoms_told`, `invalid_markers`, `quotes_kept`, `numbers_kept` | The Fidelity Report's figures, by the code that computes the page's (`report::checked()`), so the two never disagree |
| `gold_quotes_kept` | Gold's must-quotes said verbatim in the article as a reader reads it: its prose, without markers |
| `gold_terms_used` | Gold's terms the article uses |
| `facts_cited`, `core_facts_cited` | Gold's facts, all and core, some atom of whose sentences the article cites: the facts it may tell. Whether it tells them is a judge's to say (key-fact recall, P24); a fact not cited is not told, so these bound it from above |
| `length_ratio` | The article's words over the transcript's |
| `minutes`, `minutes_per_video_hour` | The job's running time, across its resumes: on a pod of its own, its GPU's |
| `calls`, `prompt_tokens`, `completion_tokens`, `escalations` | Model calls answered; every attempt's tokens; missions answered past the rung they were ordered at |

- **Gold is scored against only when reviewed, and for the snapshot read.** Silver is never scored, and gold written for another snapshot than the one the job read has anchors that may not hold. Either way the job is scored without it, and it is said.
- **A share with nothing to share is no score**: an article with no number to keep has no `numbers_kept`, rather than a 100% that would flatter an average.
- **A fact's sentences** are found as the review sheet finds a quote (`said_there()`): of the sentences said within its stretch, give or take two seconds, those its quote, aligned verbatim, covers (`sentences_saying()`).
- **The scorer is named and versioned** (`code@1`): a change to any metric's definition bumps it, so scores of different definitions are never mixed.

**`rreport eval export --label L`** writes a row per video and replicate: the run's label and tier, the video's type and caption kind, its job, its scorer, then each metric, a metric the entry has not an empty cell (`rr/eval/export.hpp`). CSV for a spreadsheet; JSON for the statistics (P25).

## The commands

| Command | What | Exit |
|---|---|---|
| `rreport eval fetch [ID…] [--again]` | Each video of the set (or those named) read once and kept; prints the line that pins it | 0; 1 when a video could not be fetched (the others are) |
| `rreport eval annotate [ID…] [--replace]` | Each video's silver drafted with the pod's models into its gold file; what the stages could not do, and what to do next | 0; 1 when a video's job is not done; 128+N on signal N |
| `rreport eval sheet ID [--out FILE]` | The review sheet, printed or written | 0; 1 when the snapshot or the gold file is missing or broken |
| `rreport eval run --label L [ID…] [--tier T] [--replicates N] [--split dev\|test]` | The split's videos through the article stages, once a replicate (3 by default, at `max`, on `dev`); the same label again takes the run up | 0 every job done; 1 a job not done, or a video not fetched; 2 a label of another run; 128+N on signal N |
| `rreport eval score --label L` | Each job done of the run scored by code; what was scored without gold, and why | 0; 1 when a job is not done yet |
| `rreport eval export --label L [--format csv\|json] [--out FILE]` | The run's scores, a row per video and replicate, printed or written | 0; 1 for no such run |

Every eval command takes `--eval-dir` (default `eval`) and `--eval-set` (default `m2`: `<eval-dir>/sets/m2.toml`). A set that does not exist yet says how one is made: from the approved candidates (`eval/README.md`).

## Decisions

- **The snapshot is the source artifact, not a format of its own.** One artifact, one hash, one reader: the eval, `rreport ingest` and the library agree on what a snapshot is without a converter.
- **The eval's own strict TOML reader** (`toml_io.hpp`), rather than sharing `rr_reason`'s tier reader: the tiers fill defaults, while these files require their values, and a shared reader waits for a third format.
- **A job's kind in the library**, rather than a database of the eval's own: annotation jobs share the stage cache (a snapshot's transcript is the video's), the call log and the battle log, and `rreport jobs` shows them beside the articles.
- **`Army::holds()`, not a copy of the army's sizing.** The stage asks the army whether a mission fits, with the estimator and presets the squad will use, instead of predicting it: a prediction would drift from the army's rules, and a mission the squad refuses fails every mission of the squad with it.
- **The oracle answers annotation by a rule a test can predict** (`rr/oracle/answers.hpp`): every sentence of five words or more is a fact, quoted whole, core when it holds a digit; the other family adds every sentence of three or four words its notes do not cite. Silver has no single right answer; a predictable one lets the tests check every step of joining.

## What waits

- **Judged scores** (key-fact recall, supported sentences, how the article reads): P24, by frozen judges, report-only until a person has calibrated them (P27). P23 scores what code can, and `facts_cited` already bounds key-fact recall.
- **Real drafts.** The oracle writes no must-quote and no term; real models will. The joining of both is tested directly.
- **Annotation end to end through the real executables.** Articles run so since P22, the oracle answering on the wire ([testing.md](testing.md#end-to-end-killed-and-resumed)). An annotation does not yet: its two missions share one answer schema, which names both prompts, so the fake engine cannot tell them apart.
- **Candidates with CC BY.** None could be confirmed from here: YouTube shows the licence only on the watch page, which this environment cannot reach (`eval/sets/candidates.toml`).

## How the tests were validated

| Suite | What it holds |
|---|---|
| `tests/unit/eval/` (79) | the set and its mistakes, each named where it is; snapshots on a fake yt-dlp: kept readably, pinned, checked, refused when changed or of the wrong kind or licence, read by the source stage as the same artifact; gold written and read back equal, times as players show them, anchors and their slack, problems; windows, sizes, draft checks, joining, the artifact; the stage on an army of oracles with refusals, misquotes repaired, ceilings, windows giving up sentences, a sentence too long; the job from snapshot to gold file, taken up again, never replacing gold; the review sheet; runs on a real library (P23): one label one run, taken up, refused when asked of another set, split, tier or number of replicates; the plan, every video once before any twice, without the test split or a video not fetched; an entry's job made from its snapshot when its turn comes, its salt the replicate, and kept; scores replaced whole, listed by video then replicate, an entry not scored listed with none; each metric on the lecture's artifacts and a hand-written gold: the Fidelity Report's figures as the page shows them, gold against the article's prose without headings or markers, a share of nothing no score, a fact's sentences at the edges of its two seconds' slack, gold not reviewed or of another snapshot never scored against, a job not done never scored, every attempt's tokens and the missions past their first rung, the minutes against the video's length or else its transcript's; CSV columns and RFC 4180 quoting; JSON |
| `tests/unit/oracle/` | the oracle's annotate and annotate_more answers, both citation forms; `RequestLog` |
| `tests/unit/agents/` | `Army::holds()` at the exact token where a mission stops fitting, thinking included, as the squad judges it |
| `tests/unit/library/` | a job's kind, written and read; the upgrade of an existing library checked by hand with `sqlite3` (old jobs read as articles; an unknown kind refused by the table's CHECK); and P23's migration on a library made at version 5 from migrations 0001–0005, again by hand: `rreport jobs` upgraded it to 6, its job kept and listed, the eval's three tables made, SQLite's integrity and foreign-key checks clean |
| `tests/component/hub_cli_test.cpp` | `rreport eval fetch / annotate / sheet` through the real executable: fetched once, then kept; no pod, needs you, with the command that takes it up again; `jobs` shows the kind; `resume` refuses an annotation; the sheet printed and written; a missing set, video, snapshot or gold file each said |
| `tests/component/end_to_end_test.cpp` | `rreport eval run / score / export` through the real executables on the oracle runtime (P23): two replicates of the lecture's snapshot run; the same label again asks the engines nothing; asked with another number of replicates, refused (exit 2); scored without gold, and said; the CSV's header and a row a replicate; the JSON written to a file |

They pass under ASan and UBSan, and the eval, agents, library, pipeline and hub suites under TSan (the stage's missions run on the army's threads), with no race reported. Writing the end-to-end test found a real defect before it shipped: windows sized by a fixed estimate made prompts the army refuses on a small engine, and the other family's prompt, with the facts found beside it, could outgrow the infantry's 4,096 tokens on a real pod. That is why windows are now checked against the army itself.

**Mutation testing** (`tests/mutants/eval.toml`, 101 mutants, plus `library.toml` 3, and 2 in `agents.toml`, 5 in `oracle.toml`): each removes one rule above, and names the test that must fail without it. All 111 are killed. P17 moved the windows to the pipeline (`rr/pipeline/windows.hpp`), and their seven mutants with them (`pipeline.toml`); the 42 left in `annotate.cpp` ran again after the move, and are all killed. Two first failed to build instead (a condition replaced by `false` left a function unused, which `-Werror` refuses); they now keep the call (`… && false`) and are killed like the rest. Writing them found what no test checked yet, now tested: a list of values given where tables belong; fact ids with letters after their number, or too long; the first hour of a video written in minutes; an unknown key in `snapshot.toml`; a draft voting core twice for one fact; the commander's answer budget; the other family shown facts from other windows; the job's salt in the seeds; a note citing a sentence first in its list. It also removed dead code: a check on empty texts that `optional_text()` already made, a key marked read when absent, and guards on empty text that `Alignment::verbatim()` already makes (a quote without words is never verbatim).

**P23** added 35 mutants to `eval.toml` (127 now): 16 for runs (a label, the spec it names, the plan, an entry's job), 15 for scores (each metric's rule, the two conditions on gold, the costs) and 4 for exports. The first run killed 33. One survived: with the code's check on replicates weakened, the table's own `CHECK (replicates >= 1)` refused a run of none with the very same error, since SQLite's CHECK failures read as `invalid_argument`; the test now checks the message, which only the code gives. One failed to build: dropping the call that records an entry's job left a parameter unused, which `-Werror` refuses; the mutant now keeps it used (`(void)runs;`). Both are killed now: all 35. Writing them found what no test checked yet, now tested: the order a run's entries are listed in (by video then replicate, not the order their jobs were made), and a fact's stretch holding a sentence its quote does not say (only the sentence saying it is the fact's). The full suite found what the eval's suites could not: the doctor's own test still expected schema version 5. It, and the CLI's, now expect the newest migration's version, so the next migration leaves them true. And clang-tidy found three reads of the transcript and Ledger A that were safe only by `report::checked()`'s contract, which a comment had said and no code checked: `scores_of()` now checks both where it reads them, and `against_gold()` is given the two it reads rather than the whole contents.

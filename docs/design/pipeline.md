# The pipeline: jobs, stages, and the stage cache

A **job** takes one video through the **stages**: `source`, `transcript`, `outline`, `atomize`, `plan`, `write`, which writes the article, and `seams`, which smooths it where its sections meet. The eval's annotation jobs run `source`, `transcript` and `annotate` ([eval.md](eval.md#silver-drafted-by-models)). Each stage turns the artifacts of the stages before it into one artifact of its own, an immutable JSON document that follows its schema (ADR-0004). This note is about what runs them, `rr_pipeline`, and the seven stages.

| Piece | Where | What |
|---|---|---|
| `Stage` | `rr/pipeline/stage.hpp` | What a stage reads and makes, its version, its schema, its fingerprint; the cache key |
| `Executor` | `rr/pipeline/executor.hpp` | Each stage of a job found done or run, checked, committed |
| `run_job()` | `rr/pipeline/job_run.hpp` | A job taken through the executor and saved: running, done, stopped, needs_user, failed |
| `Jobs`, `StageStore` | `rr/library/` | The jobs table, each job of a kind: an article, or the eval's annotation (`migrations/0005_job_kind.sql`); artifacts, the stage cache and each job's runs (`migrations/0004_pipeline.sql`) |
| `JobArmy` | `rr/hub/job_army.hpp` | A job's army, raised the first time a stage needs a model |
| `SourceStage` | `rr/pipeline/source.hpp` | A video (through yt-dlp) or a caption file, read once |
| `TranscriptStage` | `rr/pipeline/transcript_stage.hpp` | Captions into numbered sentences; punctuated by the infantry when they are not |
| `OutlineStage` | `rr/pipeline/outline_stage.hpp` | The video's sections, and the brief every later mission is given |
| `AtomizeStage` | `rr/pipeline/atomize_stage.hpp` | Ledger A: what became of every sentence, atoms or a filler label |
| `PlanStage` | `rr/pipeline/plan_stage.hpp` | The plan: which paragraph of each section tells which atoms, and which atoms only another tells |
| `WriteStage` | `rr/pipeline/write_stage.hpp` | The article: every section's paragraphs, every atom told and cited, checked as Ledger B |
| `SeamsStage` | `rr/pipeline/seams_stage.hpp` | The article smoothed where its sections meet: the two paragraphs at each seam rewritten together, kept where no worse |
| drafting | `rr/pipeline/drafting.hpp` | What the stages that write prose share: an atom as a writer is shown it, an answer's room, a draft's checks |
| judging | `rr/pipeline/judging.hpp` | A draft's sentences read against their atoms, and two drafts compared, by a judge of another family |
| windows | `rr/pipeline/windows.hpp` | A transcript cut into windows of whole sentences, each as large as its engine holds |
| sections | `rr/pipeline/outline.hpp` | Sections from chapters, from pauses, from outlines agreed on; long ones cut; briefs joined |
| readings | `rr/pipeline/readings.hpp` | What models made of a window, voted on sentence by sentence and reduced to one reading |
| grouping | `rr/pipeline/grouping.hpp` | A section's atoms told in the order said, a paragraph at a time, where no model planned them |
| asking | `rr/pipeline/asking.hpp` | What every stage that asks models says the same way: the video, a prompt's fingerprint, an answer's problems a few at a time, why a mission went unanswered |

## The cache key

A stage's key is the BLAKE3 hash of the canonical JSON of everything its result depends on:

| Part | Why |
|---|---|
| its name and **version** | the version goes up when the stage makes something else from the same inputs: a new rule, a fixed bug |
| its artifact's **schema**, as this binary has it | an artifact written under another schema is never served (ADR-0004) |
| its **fingerprint**, as the stage says | its prompts and their content hashes, the models it asks (`army_fingerprint()`), its settings |
| its **inputs**: the hashes of their artifacts | not the keys that made them |
| the job's **salt**, for a stage that samples | a new salt draws new samples; the same salt finds the old ones (ADR-0020) |

Two choices follow from this:

- **Inputs by content, not by key.** A stage keyed on its inputs' keys would run again whenever anything upstream changed. Keyed on their artifacts, it runs again only when what it reads changed. A new punctuation prompt that punctuates a video's manual captions the same (it is never asked) changes the transcript stage's key, but not its artifact. So the outline, the atoms and the article are still found done.
- **The models by file, not by name.** `army_fingerprint()` writes each engine's model id, the SHA-256 of the file it serves (`Engine::checksum`, from the runtime's health report), its roles and its two sampling presets, in the order of their ids. The engines' sizes are left out: four slots answer faster than two, not otherwise. A catalog that re-pins a model id to a new file changes every key that names it.

A stage finds the fingerprint its key was made from in `StageContext::fingerprint` when it runs, and makes its result from the same. The source stage relies on this: it keeps a caption file only if its bytes still hash to the fingerprint's hash.

## Running a job

For each stage, in order, the executor:

1. gives it its inputs, the artifacts of the stages it reads (made or found earlier in the run);
2. stops if a stop was asked (`Errc::cancelled`);
3. takes its fingerprint and makes its key;
4. looks the key up. **A hit is a stage done**, by this job before it stopped, or by any other job. Its artifact is read back (its BLAKE3 checked), and the job's run is recorded as found done, with the findings kept with the cache entry;
5. otherwise runs it, **checks its artifact against its schema**, and commits. The artifact (kept once under its hash), the cache entry and the job's run go in **one transaction**: a crash leaves all three or none.

An artifact that breaks its schema is the stage's own bug (`Errc::internal`), and is never kept: every stage after it would build on it.

The first failure ends the run. It is recorded on the stage's run, under its key if it was known. `run_job()` then saves the job with the status the error implies:

| Error | Status | Next |
|---|---|---|
| `cancelled` (Ctrl-C), `budget_exceeded`, or transient (`network`, `timeout`, `unavailable`, `rate_limited`) | stopped | `rreport resume`: a stop, a ceiling, a pod or a network down for now |
| `needs_user` | needs_user | the user acts (a bot check, no runtime token, no recorded engines), then resumes |
| anything else | failed | the input or the code is wrong |

A job is saved **running** before its first stage starts, so a process that dies leaves it running, where `rreport jobs` shows it and `rreport resume` takes it up. The time each run takes is added to the job's own: the wall-clock ceiling counts it across resumes (ADR-0018). A job that is done forgets its memo of model answers: its artifacts are the truth from then on (ADR-0020).

### A result kept for its job alone

A stage's result can depend on its job, not only on its key. The case today is a mission that a **job ceiling** stopped (ADR-0018): its window was left unpunctuated, and a job allowed more would have punctuated it. Such a result is committed like any other, but `StageOutput::reusable = false` leaves out its cache entry. The job keeps it and publishes its findings, and no other job is ever served it.

## The army, raised when needed

A pod costs money from the minute it starts. So a stage is given **the means to raise the army** (`StageContext::army`, a function), not an army, and a job whose stages need no model never starts the pod. The first stage that asks one raises it: the transcript, for captions that need punctuating; otherwise the outline, which reads every video with the commander when a pod is configured.

`JobArmy::raise()` raises it once per job:

1. **The pod's engines.** In live and record mode they come from the runtime's health report, once every engine is ready. A pod RunPod manages is started if it is stopped, as `rreport pod start` would. In replay mode no runtime is called: the engines come from `engines.json`, beside the recordings, which record mode writes as it raises its army. A replay therefore raises the same army as the one recorded.
2. **The stack every call goes through** (`ModelStack`: the runtime and nowhere else, retries, accounting into `llm_calls`, recording), and over it **the job's memo**, keyed by each model's file checksum: a resumed job asks again none of the calls it already made.
3. **The army**, with the job's ceilings, logging each mission in `missions`.

**The job's ceilings** (ADR-0018) bound what it may spend: completion tokens and wall-clock time, each **per hour of its video** (`[jobs]`: 5 million tokens and 4 hours to start with, about 5 to 10 times what `max` is expected to spend; runbook R3 measures it). A three-hour podcast may spend nine times what a twenty-minute talk does. A video shorter than a quarter of an hour counts as a quarter of an hour: some costs do not shrink with it (the outline thinks over the whole video, three times at `max`), and a ceiling below them would stop every short video. The video's length is asked when the army is raised. By then it is known, since the source stage, which comes first, needs no model: the length the site gives; else where the captions end; else, for captions without times, their words at 150 a minute. The army says what it may spend (`army: the job may spend 2,500,000 completion tokens and 2 h 00 min, for 30 min of video`), and the job's time before this run counts against it.

A failure is returned to the stage that asked, and the next stage that asks tries again: a pod that did not come up may come up later. A stage that needs the army also raises it in its fingerprint, since its key names the models that will answer.

Without a runtime configured, and outside replay mode, the CLI gives the stages no means to raise an army at all. A stage that needs a model then does what it can without one, and says so in a finding. The atomize stage can do nothing without one: the job stops there, `needs_user`, and `rreport resume` takes it up once a pod is set up.

## The source stage

The source stage reads what the user gave `rreport ingest`, once, into the `source` artifact (`schemas/artifacts/source.json`):
- the video as its page describes it: title, description, channel, date, duration, language, licence, tags and chapters;
- its captions, whole, as downloaded.

**Every stage after it reads the artifact, never the network.** A resumed job reads what the job read, even if the video changed since, and only the first stage can fail for the network.

| Given | Read | Key |
|---|---|---|
| a web address (`http://`, `https://`) | yt-dlp: the description, then the captions in the language spoken ([transcript.md](transcript.md#which-captions)) | the address |
| anything else: a caption file | its bytes; its name says the format (`.vtt`, `.srt`, `.json3`, `.txt`) and, without the extension, the title | its name and the BLAKE3 of its bytes |
| a file named `*.source.json`: a source read earlier and saved whole, such as an eval snapshot ([eval.md](eval.md#snapshots)) | its bytes, checked against the source schema and kept as they are, so the stages after it run as they would on what it was read from | as any file |

- **A video is read once per library.** A second job for the same address finds the first one's source. Two addresses of one video (`youtu.be`, `youtube.com`) are fetched once each, but their artifacts are equal: the artifact keeps the video's own address, not the one given. So the keys of every stage after them are equal too.
- **A file is read by content.** An edited file is read again; a copy under the same name, anywhere, is not. The job keeps the file's absolute path, so it can be resumed from any directory.
- **The bytes the key names are the bytes kept.** The fingerprint hashes the file, then `run()` reads it again. If the two differ, the file changed in between: `Errc::conflict`, never a source kept under another source's key.
- **Empty texts are null.** An empty tag says nothing and is dropped; a page with no title is titled by its ID.

A live or upcoming video has no captions yet: `choose_captions()` makes it `Errc::unavailable`, so the job stops, to be resumed once the stream has ended.

## The transcript stage

The transcript stage reads the source's captions ([transcript.md](transcript.md)) and cuts them into the transcript's sentences. What it does depends on the captions, and its fingerprint says which:

| Captions | Fingerprint | What happens |
|---|---|---|
| punctuated (`punctuated()`: a sentence end every 40 words or fewer) | `{"punctuation": "captions"}` | cut into sentences as they are; no model is asked, and the pod is never started |
| not punctuated, no pod configured | `{"punctuation": "none"}` | cut where the speaker paused, 60 words at most, with a finding that says so |
| not punctuated, a pod | `{"punctuation": "models", "prompt", "prompt_hash", "window", "army"}` | punctuated by the infantry, a window at a time |

Captions that hold no word at all (`[Music]` only) are an error, `Errc::not_found`: nothing after the transcript could be done.

### Punctuation, a window at a time

A whole video fits no call, and a model punctuates a page better than a book. So the words are cut into **windows** of about 1,000 bytes. UTF-8 bytes stand in for tokens: about four bytes a token in English, three in Chinese. Each cut falls **where the speaker paused longest** among the cuts that leave between 600 and 1,400 bytes, since a pause is where a sentence most likely ends. Ties go to the cut nearest 1,000 bytes. A word too long for any window is a window of its own.

A window cut mid-sentence would be punctuated as if it began or ended there: a capital after the cut, a full stop before it. So **each window is sent with its context**: up to 200 bytes of the words before it and after it. The model punctuates all of it, and only the window's own words are kept. Every word is in exactly one window, and every word at a window's edge was punctuated with the words around it in view. The test that shows it punctuates thirty sentences in about ten windows, and gets every sentence of the script back, edges included.

Each window is a **`punctuate` mission** for the infantry, all of them sent at once to the army, which runs as many side by side as the infantry holds:
- **Temperature 0.** Punctuation is not a matter of taste: the most likely answer, the same every time.
- **No salt.** The stage does not sample, so the job's salt is not in its key or its seeds. Every job, and every replicate of the eval, reads one transcript of a video.
- **The answer is checked before it is taken.** Its punctuation is laid onto the window's words by projection (`project()`), which can change no word. The checks ask that the answer:
  - keeps the words: at most one in twenty, and at least two, changed, added or left out. Those are never taken anyway, but a model that rewrites has not just punctuated, and its punctuation is doubtful;
  - is punctuated, by the same test that sent the captions to be punctuated.

  A failing answer is sent back with its problem, said as what to do instead ("Keep every word exactly as it is, …", "Mark where each sentence ends, …"). Then the mission climbs the escalation ladder (rr/agents/ladder.hpp).
- **A window no model punctuated keeps the captions' form.** A finding names the minutes it covers and why: no answer passed its checks (with the last problems), the models declined, or a job ceiling came first. Its words still reach the transcript, cut at most 60 to a sentence: nothing said is lost.

### The prompt

`prompts/punctuate` is rReport's first prompt ([prompts.md](prompts.md)).
- **Its system part** holds the rules, a synthetic example, and the video's title, channel and description. The description holds the spelling of names and terms. It is cut after a line, at 1,500 bytes, before the links and sponsors that tend to follow. The system part is the same for every window of a video, so the engine's prompt cache serves it from the second window on.
- **Its task part** holds the excerpt alone.
- **The answer** is `{"text": …}` (`schemas/missions/punctuate.json`).
- **The example** says why it is there: "were" stays "were". The model adds punctuation and corrects no word, not even one it thinks misheard.

The oracle answers `punctuate` missions from the script a synthetic video was read from (`rr/oracle/answers.hpp`). It finds the excerpt's words in the script and returns the script's text for them. Without a script, it ends a sentence every twelve words, which passes the checks, so a pipeline runs end to end on any captions.

## What every stage that asks models shares

**Windows** (`rr/pipeline/windows.hpp`). A stage that asks about a transcript asks a window at a time: a model reads a few pages better than a book, and an engine holds only so many tokens. Every such stage cuts the same way:
- a window is whole sentences, shown a line each, `S12 [3:04] What was said.`;
- it is sent with **context** on either side, up to three sentences, as many as fit 1,000 bytes of lines, so that its first and last sentences are read as they were said;
- it is **filled** with sentences up to a size the stage chooses, then **given up from its end** while the engine would not hold its mission (`Army::holds()`, the very test the squad applies). The largest window the engine holds is found by **bisection**. So no mission is ever refused for its size, whatever the prompt around the sentences, and a sentence too long for its engine even alone is set aside for the stage to report.

Holding is monotone in a window's end: giving up a sentence takes its line out of the prompt and puts at most one line of context in, and the answer shrinks with it. That is why bisection finds the window that giving up one sentence at a time would; a test compares the two over six engine sizes and three fill sizes, with sentences too long for context and for the engine among them. The eval's silver drafts (P16) were the first to read this way, and now read through the same code.

**Asking** (`rr/pipeline/asking.hpp`). What a prompt's shared part shows of the video (its title, its channel, and its description cut after a line at 1,500 bytes, before the links and sponsors that follow), a prompt's place in a key (its label and content hash), and why a mission ended without an answer: the models declined; the job reached a ceiling first; or no answer passed the stage's checks, with the last problems. Findings say it the same way in every stage. An answer's problems are sent back **eight at a time at most**, then "and 3 more like them" (`Problems`): enough to fix, few enough to read. A window that dropped forty sentences gets eight named, not forty lines that bury the rule.

**Tiers** (`job_tier()`). A stage finds its job's tier through `StageContext::tiers`: the shipped tiers (`reason::builtin_tier()`) unless the context says otherwise. Tests use it for the settings no shipped tier has (a tier that takes no chapters, one that asks for waves), and the eval's sweeps will use it for their variants (P26). A key holds a tier's settings, never its name, so a variant never takes a shipped tier's results.

## The outline stage

An article is written a section at a time, and every mission after this stage sees its own stretch of the video and the video's **brief**, no more (plan §3.7). The outline stage writes both ([knowledge.md](knowledge.md#the-outline)). It decides two things: where the sections are, and what they say.

### Where the sections are

Best first:

| Sections from | When | How |
|---|---|---|
| `chapters` | the tier takes chapters (every shipped tier does) and the video has two or more | the creator's own cut. A chapter starts at the sentence that starts closest to its time, the earlier of two as close, so a speaker a moment early or late is still its first sentence. The first chapter starts at S1, whatever its time says. A chapter that would start where the one before does, or before it, is dropped |
| `models` | a pod, and no chapters to take | the commander's outlines (`prompts/outline`), as many as the tier's samples (3 at `max`), thinking (below). A section starts where enough of them start one within a sentence or two of each other (`reason::boundary_consensus()`: at `max`, two of three within two sentences), at the place closest to them all. One sample's whim is not a section. The title is that of the first sample that starts a section exactly there |
| `pauses` | no pod; or no outline came back | cut where the speaker paused longest, until no section is longer than eight minutes; each titled by its stretch, `12:30–19:45` |

Whatever drew them, **no section runs longer than eight minutes**, what a writer drafts in one piece (plan §5, stage 8). A longer one is cut where the speaker paused longest, among the cuts that leave each part a quarter of it at least, so that it is cut in balanced parts, not trimmed. Between pauses as long, and when no cut leaves a quarter, it is cut nearest its middle; on a tie, at the earlier. Its parts keep its title, numbered: `Radium (2 of 3)`. A single sentence is never cut.

**How the commander outlines.** It reads the whole transcript at once when it holds it: sections are a judgment about the whole. It thinks within the tier's budget (16,384 tokens at `max`, 8,192 below), but never more than half of what it holds: a budget that left no room for the transcript would leave nothing to think about. A transcript too long for one reading, such as a 90-minute podcast on a 48 GB pod at `max`, is read in windows as large as it holds, each with the sentences just before it as context, each told to list only the sections that start among its own sentences, and to list none when the talk does not turn. Each window's samples are agreed on by themselves: a quorum counts the samples that came back for it.

**What is sent back.** An outline is checked before it is taken (the repair loop and the escalation ladder do the rest, `rr/agents/army.hpp`):
- a section that starts outside its own sentences;
- sections out of order, or one listed twice;
- a first window whose first section is not S1;
- a title of more than twelve words;
- more than a section a minute, plus one: a section at every pause is no outline.

**What no outline covers** is cut where the speaker paused, with a finding that says which sentences and why. A sentence too long for the commander even alone starts a section; sentences set aside one after another are one stretch, cut at pauses, with one finding: an engine too small to hold any of them leaves the video as it would be without a pod, not one section a sentence.

### What the sections say

With a pod, the commander **describes each section on its own** (`prompts/describe`): who speaks in it, by the name the video gives them; the section's own words for things, with their meanings as the video explains them; its gist in one or two sentences; and what the video is, as far as the section shows. The descriptions **join into the brief** (`join_briefs()`):
- the genre most sections name, the first named on a tie;
- the speakers and the words, each once by its folded form ("ADA LOVELACE" is "Ada Lovelace"), a role or a meaning filling an empty one and never replacing one;
- at most 12 speakers and 40 words, those the most sections name. A word every section uses matters more than one a section mentions, and it is what a model listing too much lists least often.

From the sections' gists, in order, the commander then writes **the video's gist**, a paragraph (`prompts/brief`).

Describing is reading, not judging, so it is asked **without thinking**: reasoning models invent more when they summarize (Vectara's leaderboard: 14.3% for R1 against 3.9% for V3). Only the outline, a judgment over the whole, thinks. The eval decides whether that holds for these prompts (P26).

**What is sent back.** A description that names a speaker neither the section nor the video's title, channel or description names; lists a word the section does not say, or more than fifteen; or gives a gist of more than sixty words. A video gist of more than 150 words. The words are compared folded, so case, accents and punctuation do not count.

**What no description covers** leaves its section without a gist, and its speakers and words out of the brief, with a finding. The video's gist is then written from the gists there are, a section without one listed by its title; with no gist at all, the brief has none, and a finding says why.

### The key, and the prompts

The stage samples, so the job's salt is in its key. Its fingerprint holds the tier's outline settings and the longest a section may run; with a pod, the three prompts' labels and hashes, the army, what the commander holds, and the context windows are read with. Without a pod the stage cuts at pauses, or takes the chapters, and says the brief is empty; a pod configured later makes another key, so the stage runs again with models.

| Prompt | Asks | Answer |
|---|---|---|
| `outline` | where sections start among the sentences to outline, with titles; the first window is told its first section starts at S1, and the others that a section may start nowhere among their sentences | `{"sections": [{"first", "title"}]}` |
| `describe` | one section: who speaks, its words, its gist, the genre | `{"speakers", "lexicon", "gist", "genre"}`: the evidence first, then the gist |
| `brief` | the video's gist from the sections' titles and gists, in order | `{"gist"}` |

All three share the video's title, channel and description in their system part, and each example is synthetic. The oracle answers each by a rule a test can predict (`rr/oracle/answers.hpp`): a section starts at S1 and at each sentence that opens with "Now," or "Next,"; a speaker for each "I'm" and the name after it; a word for each acronym; the first sentence as a section's gist; and "A video in 4 sections, from … to …" as the video's.

## The atomize stage

**Ledger A.** An article is only as faithful as what it is written from. So rReport never asks a model to summarize: it asks what **every sentence** says, and code checks that none is missing (ADR-0005). Each sentence becomes one or more **atoms**, units of what was said that a faithful retelling must keep, or a **filler label** from a closed list (a disfluency, a greeting, a repetition, a sponsor, a decorative remark) that says why it holds nothing to retell. The article is later written from the atoms, and must tell every one of them (Ledger B, P20). The ledger's form is in [knowledge.md](knowledge.md#atoms); its checks in [fidelity.md](fidelity.md).

The stage takes five steps (`rr/pipeline/atomize_stage.hpp`):

### 1. Reading a window

The infantry reads the transcript in **closed-world windows**: about 2,500 bytes of sentence lines, then given up from the end until the infantry holds the mission (the shared windows above), with **two sentences of context** on either side that it may read but not cite (the plan's ±2). A window is a closed world: an answer may write rows for its own sentences only, and the checks hold it to that.

The answer is a row a sentence, in order (`schemas/missions/atomize.json`):

```json
{"sentence": "S9", "filler": null, "atoms": [
  {"also": [], "names": ["Marie Curie"], "terms": [], "text": "In 1902 Marie Curie called radium \"the most precious thing\".",
   "kind": "number", "modality": "asserted", "register": "main", "intensity": "medium",
   "speaker": null, "attributed_to": null, "must_quote": "the most precious thing"}]}
```

**The fields come in the order a careful reader works** (plan: reason, then format). The evidence first: the later sentences the atom also draws on, then the names and the video's words it uses, copied as said. Then the atom's own words. Then the judgments: its kind, its modality (hedged, reported, hypothetical…), its register, its intensity, who says it, whom it reports, and the words it must quote. A constrained decoder writes fields in schema order (P1), so the model has written down what the sentence says before it labels it.

**An atom is written in the row of the first sentence it draws on**, and names the later ones in `also`: a thought that runs over two sentences is one atom citing both, never two halves. A sentence whose thought an earlier row's atom already holds has no atoms of its own, and is accounted for all the same.

**The prompt.** Its system part holds the rules, a synthetic example (five sentences: a disfluency, a number with an aside, a definition to quote, a thought that runs on), and the video's **brief** from the outline stage: its gist, its speakers with their roles, its words for things with their meanings. It is the same for every window, so the engine's prompt cache serves it from the second window on (P7's layout rule). The task part holds the window's section titles, its context, its sentences, and what the mode adds (below).

**Without thinking, at every shipped tier.** Labelling is narrow and schema-bound; thinking adds cost, and reasoning models invent more when they restate (the Vectara figures above). A tier may turn it on; the eval decides (P26).

**How large an answer may be.** Its JSON takes about 300 bytes a sentence besides the words it restates: the row, and an atom or two with their eleven fields. The prompt's own example takes 1,180 bytes for five short sentences. The stage allows 320 bytes a sentence plus the sentences' own bytes, counted at the estimator's three bytes a token, which errs high, plus 128 tokens, between 512 and 8,192. An answer cut short is asked again whole, which costs more than room left unused; and since the window planner shrinks a window until its answer fits, a generous answer only makes windows smaller.

**What that means on a pod.** On the 48 GB profile, each of the infantry's sixteen slots holds 4,096 tokens. The rules, the example and the brief take about 1,500 of them, so a window holds a dozen sentences or so: the test lecture is read as windows of 15, 12 and 3 sentences, against two windows on an engine that holds 32,768. Smaller windows cost more calls and more repeated system prompts (served from the cache), not accuracy; but runbook R1 should weigh unified KV for the infantry too, which would let one sequence use the pool's 65,536 tokens.

### 2. What is sent back

Each answer is checked as Ledger A is, on its own window (`fidelity::check_ledger()`), and sent back with what to do, the repair loop and the escalation ladder doing the rest (`rr/agents/army.hpp`):

| The answer | What it is told |
|---|---|
| writes a row for a sentence outside its window | `S99 is not among the sentences to read (S1–S27): write rows for those only` |
| writes a sentence twice, or out of order | `S2 is written again, or out of order: one row a sentence, in order` |
| leaves a sentence with neither atoms nor a label | `S4 is not accounted for: give it one or more atoms, or a filler label (…)` |
| labels a sentence and gives it atoms | `S1 is labelled greeting and has atoms: it is one or the other` |
| names an `also` that is not a later sentence of the window | `S7's atom 1 also names S40: name only later sentences among those to read` |
| gives a number its sentences do not say | `S4's atom 1 says "4", which its sentences (S4) do not: they say "3"` |
| quotes words that are not the sentence's, word for word | `S9's atom 1's quote "the most valuable thing" is not what was said: its sentences say "the most precious thing"` |
| labels as filler a sentence that holds a number, a name or one of the video's words | `S6 is labelled decorative, yet holds the term "half-life": re-examine it, and give it atoms unless it carries nothing` |
| gives a speaker neither the brief nor the sentences shown name | `S2's atom 1 gives its speaker as "Pierre", whom neither the video's speakers nor these sentences name: give null when the transcript does not tell` |

**Atoms are named by where they were written.** The ledger's checks number atoms A1, A2, …, but an answer has no A3: it wrote rows. So every message is translated before it is sent: `A3` becomes `S12's atom 2`, the second atom of the row S12. A model fixes what it can find. Only the atoms a finding concerns are renamed (`Finding::atoms`), each as a whole word: a quoted sentence that happens to say "A2" keeps its words, and `A1` is never read inside `A18` (`S22 is labelled disfluency and told by S2's atom 1, S21's atom 1`).

**A warning is not sent back.** A name the transcript does not say (`name-said`) is worth a reader's look, not a repair: the answer is taken, and the finding is published with the ledger.

**The video's words count.** A filler may hold none of the video's own words, its brief's lexicon, besides the atoms' terms (`check_ledger(…, lexicon)`, new here): a window whose atoms happen not to use "half-life" must still not label "Its half-life is long." decorative.

**A speaker is the brief's or the window's.** Compared folded, the brief's "Ada" accepts "ada"; a name the sentences or their context say is accepted too ("Marie Curie" in the sentence itself). Anything else is a guess, and `null` is the honest answer when the transcript does not tell.

### 3. Votes, and what they do not settle

At `max` each window is read **three times**, the samples drawn at once from one reading of the prompt where the engine holds them (P3), each with its own seed. Self-consistency over samples beats asking a model to check itself at equal compute (Wang et al., 2023; Singhi et al., 2025). The readings are reduced to one by pure functions (`rr/pipeline/readings.hpp`), sentence by sentence:

- **What is voted on is what a sentence became, not its words.** Free text never agrees, so a sentence's ballot is its **verdict**: `filler greeting`, or the sorted kinds, modalities and must-quote flags of the atoms that cite it, `atoms claim/asserted, number/hedged/quote`. A hedge read as a fact, a must-quote missed, a sentence dropped as filler: those are what a vote should catch. Register and intensity are left out: readers place them on a scale and differ without either being wrong, so voting on them would contest most sentences and decide nothing.
- **One reading, never a patchwork.** An atom may cite several sentences, so the window's atoms are never stitched together from different readings: that would split one atom, or tell it twice. They come from its **base reading**, the one whose verdicts win the most votes (the first on a tie). A verdict wins with the tier's quorum: two of three at `max`.
- **Where the base is not the winner**, because it lost the vote or the vote is tied, its sentences are **unsettled**. They are grouped into stretches, each grown until no atom of the base crosses its edge, and joined where they meet: taking a stretch out of the base takes whole atoms.
- **The commander reads each stretch again, thinking** (`contested_to_commander`, on at `max`), shown what each reader made of it:

  ```
  Reader 1:
  - S5: fact (hedged): Maybe the scales are older than the lab.

  Reader 2:
  - S5: claim (asserted): Maybe the scales are older than the lab.
  ```

  It is told to agree with a reader only where the sentences agree with them, and its reading replaces the base's on the stretch. It thinks within its engine's own budget, but never more than half of what it holds (the outline's rule). A stretch the commander cannot hold, or does not settle, leaves the base standing there, with a finding that says so; so does a tier that does not ask the commander, and a pod that serves none (the 24 GB infantry profile).

### 4. Asked again

At `thorough` and `max` (`recall_pass`), a sentence whose atoms hold **less than half its words** is asked again (`coverage()`: the share of its words found in its atoms' texts, names, terms and quotes, folded). The infantry is shown the sentence alone, with the atoms it has, and asked for what they miss:

- the answer's atoms are **added**; nothing is taken away, so the pass can only add what was said;
- a sentence asked again is **never filler**: it was read as saying something;
- an atom that **repeats** one the sentence has (its words within a quarter of the other's, `reason::word_distance()`) is sent back: write only what is missing.

The sentences are shown alone, without context, since the ones asked again from a window need not follow one another; the atoms they have say what they are about. A sentence its atoms hold whole is never asked: the pass costs nothing where the first reading was complete.

### 5. The ledger

The windows join into one ledger, atoms renumbered `A1…An` in the order said, and it is **checked whole**. What still fails is a finding, never hidden. A window no answer read (the models declined, a ceiling came first, or no answer passed its checks) leaves its sentences unaccounted for, and a finding says why, once, for the window: not again for each sentence it left unaccounted.

**What the stage says while it works**: `atomize: the infantry reads 2 windows, 3 times each`; `atomize: the commander settles 1 stretch the readings disagree on`; `atomize: the infantry reads 1 window again, where atoms hold little of what was said`.

**The key.** The stage samples, so the job's salt is in its key. Its fingerprint holds the tier's atomize settings, the prompt's label and hash, the army, what the infantry and the commander hold, the windows' size and context, the least coverage and what counts as a repeat.

**What it refuses.** Without a pod, its fingerprint is `Errc::needs_user`, and the job waits. A tier that asks for more waves of samples where the first disagree (`atomize.step`) is refused, `Errc::unsupported`: no shipped tier asks for them, and drawing them is a Phase 6 experiment (P28). A transcript without sentences is `invalid_argument`; an outline that does not cover its transcript is the outline stage's bug, `internal`.

**The oracle** answers every mode by a rule a test can predict (`rr/oracle/answers.hpp`): filler words alone are a disfluency, greeting words a greeting; any other sentence is one atom, the sentence itself, running on into the next when it ends without a full stop; a digit makes a number, "?" a question, "maybe" or "I think" a hedge; runs of capitalized words are names, acronyms terms, and the words between double quotes are to be quoted. Asked again, it writes the atom it would have read, unless an atom shown holds it already.

## The plan stage

**Why plan apart from writing.** Ledger A holds what every sentence said; the article must tell all of it (Ledger B). A writer that decided what to tell as it wrote would drop an atom unseen, and the drop would surface only in the article's check, after the prose, with a repair rewriting prose to fit it in. Deciding first, in a plan that code checks, catches it before any prose exists: the writer is handed each paragraph's atoms as a closed list (P20). Planning is also where thinking pays (plan §5): grouping what belongs together and finding what is only said again is judgment over many atoms, every answer of it checkable. Drafting, where reasoning models invent more (the Vectara figures above), comes after and thinks less.

The stage takes four steps (`rr/pipeline/plan_stage.hpp`):

### 1. A section at a time, a closed world

Each section of the outline is planned on its own, from its atoms: those whose first sentence it holds, in the order said. The commander plans it **thinking**, within the tier's budget (16,384 tokens at `max`, 8,192 at `thorough`) and never more than half of what it holds, the outline's rule. Sections are planned side by side, on the commander's slots.

The prompt's shared part shows the video (its title, channel and gist) and **the article's sections in order**, each with its start and gist: every section is planned knowing what the others cover, and the part is the same for every section, so the engine's cache serves it. The task shows the section and its atoms, a line each, their labels before their words:

```
A3 [2:00] claim, hedged: Maybe the scales are older than the lab.
A6 [4:00] number: In 1902 Marie Curie called radium "the most precious thing". (to quote: "the most precious thing")
A26 [14:00] claim, hedged, aside, said by Ada Lovelace, reporting Pierre: I think the numbers mean we need better scales, Pierre.
```

Only what planning needs is shown: the kind; the modality and the register where they are not plain (a question's modality is its kind, said once); who says it and whom it reports; the words to quote. Names and terms are the writer's concern.

**The answer comes in the order a planner works:** each paragraph's purpose (what a reader learns from it) before its atoms, each duplicate's reason before its atoms, and the heading last, once the section is planned. A paragraph tells twelve atoms at most, a purpose is a sentence of thirty words at most, a heading twelve words.

### 2. What is sent back

| The answer | What it is told |
|---|---|
| names an atom of another section | `A8 is not among the atoms to plan (A1–A7): plan those only` |
| tells an atom twice | `A1 is told twice: tell each atom once, in one paragraph` |
| leaves an atom out | `A7 is in no paragraph: tell it, or declare it a duplicate of the atom that says all it says` |
| declares an atom a duplicate of itself, or of one no paragraph tells | `A7 is declared a duplicate of A9, which no paragraph tells: name an atom a paragraph tells, or tell it` |
| tells an atom and declares it a duplicate | `A1 is told and declared a duplicate: it is one or the other` |
| declares a duplicate that would lose what it says | `A2 is declared a duplicate of A1, yet it says "3", which A1 does not: tell it` |
| writes a paragraph of more than twelve atoms, a purpose of more than thirty words, a heading of more than twelve | `paragraph 1 tells 13 atoms: split it, 12 at most` |

**A duplicate must lose nothing.** Declaring one is the only way an atom goes untold, so each is held to `fidelity::duplicate_problems()` ([fidelity.md](fidelity.md#duplicates-what-telling-the-other-would-lose)): a number, a name, a term or words to quote that the other lacks, or another modality, speaker or source, each sent back. When in doubt, an atom is told: a duplicate refused costs a sentence of the article, one wrongly accepted loses what was said.

**The order of the talk is asked for, not checked.** An atom may move within its section where a thought reads better whole, and the purpose says why. The article's order check warns where one did ([fidelity.md](fidelity.md#order-the-fewest-to-move)).

### 3. A section too long, in parts

A section the commander does not hold is planned in **parts**, each the most atoms it holds from where the last one ended, found by bisection as windows are. Each part's task says which atoms it plans and that the others are planned apart; the parts' paragraphs are joined in order, and the first part names the heading. An atom more than the commander holds even alone is told in the order said, with a finding. On the 48 GB profile the commander holds 32,768 tokens and thinks with half of them. A section runs eight minutes at most, about a hundred atoms, and an atom takes some 85 tokens, its line and its share of the answer: under 10,000 tokens with the prompt's rules, so parts are for smaller commanders.

### 4. What no answer planned

A section, or a part, that no answer planned (the models declined, a ceiling came first, or no answer passed the checks) is **told in the order said**, a paragraph at a time (`rr/pipeline/grouping.hpp`), with a finding that says why: `section 2, "The samples": its atoms are told in the order said, a paragraph at a time: the models declined to plan it`. Grouping keeps every atom and the talk's order: a new paragraph after six atoms, and wherever the speaker or the register changes. A grouped paragraph has no purpose, and a section no answer planned is `planned_by: grouping`.

**At `fast`** every section is grouped (`plan.mode = "group"`): no model is asked and no pod is needed, and no finding is published, since that is the tier's choice. A section whose sentences are all filler has no paragraph, and nothing to ask.

**What the stage says while it works**: `plan: the commander plans 3 sections` (`, in 4 parts` where it took more missions than sections), counting the sections it is asked about, and nothing when it is asked about none; `plan: the atoms are told in the order said, a paragraph at a time`.

**The key.** Its fingerprint holds the tier's plan settings and the limits (atoms a paragraph, words a heading and a purpose, atoms a grouped paragraph), and, where the commander plans, the prompt's label and hash, the army and what the commander holds. It samples: the job's salt is in its key.

**What it refuses.** Without a pod, at a tier that plans with the commander, its fingerprint is `Errc::needs_user`. An outline that does not cover its transcript, or a ledger with an atom said past the transcript's end, is a bug of an earlier stage: `internal`.

**The oracle** plans by a rule a test can predict (`rr/oracle/answers.hpp`): three atoms a paragraph, in order, each purpose naming its atoms ("Tells A1 to A3."); an atom shown exactly as an earlier one, labels and words, declared that one's duplicate; the section's title as its heading.

## The write stage

**Prose, held to the atoms.** The plan says which paragraph tells which atoms; this stage writes them as prose a reader wants to read, and code holds every sentence to them. A sentence ends with the markers of the atoms it tells (`[A12]`, ADR-0006), so each one traces back to what was said and when, and the draft is checked as Ledger B checks the article (`fidelity::check_article()`, [fidelity.md](fidelity.md)). Revising works with feedback from outside the model (CRITIC, Chain-of-Verification, above): a draft that fails is sent back with what failed, never asked to check itself.

The stage takes six steps (`rr/pipeline/write_stage.hpp`):

1. **A section at a time, a closed world.** The commander writes each section from its plan: each paragraph's purpose, and its atoms in full, their labels before their words and, after them, what must reach the paragraph (the words to quote, the names, the terms). The shared part shows the video, its speakers and words, and the article's sections in order. The drafts are drawn at once where the commander holds them: one at `fast`, two at `thorough`, four at `max`. **Without thinking** at every shipped tier: reasoning models invent more when they restate (the Vectara figures); a tier that thinks gets the commander's own budget, within half of what it holds. Sections are written side by side.

   ```
   Paragraph 1: Tells A1 to A3.
   A1 [0:30] claim: I'm Ada, and today we weigh radium. Names: Ada.
   A2 [1:30] number: The NRC lent us 3 scales for it. Terms: NRC.
   A3 [2:00] claim, hedged, high: Maybe the scales are older than the lab.
   ```

2. **What is sent back**, up to the tier's repair rounds (one, two, three):

   | The draft | What it is told |
   |---|---|
   | writes a heading | `write no heading: the article has its own` |
   | writes a list, a quote set apart, code | `write paragraphs of prose only: no lists, no quotes set apart, no code` |
   | writes more or fewer paragraphs than the plan's | `write 3 paragraphs, one for each given, not 2`, and no more about where its atoms are: which paragraph is which cannot be told |
   | tells an atom in another paragraph than its own | `A3 is told in paragraph 2: tell it in paragraph 1, as given`, numbered as in the section, a part's too |
   | tells an atom again in another paragraph | `A1 is told again in paragraph 2: tell it in paragraph 1 only`: a paragraph tells its own atoms and no others, as the prompt says |
   | writes a sentence that cites no atom | `the sentence "It was a long day." tells no atom: cite the atoms it tells, or leave it out`; a long one is quoted to its first 120 bytes and `…`, cut between characters, never inside one |
   | breaks Ledger B, held to the section's atoms | `A4 is not retold: tell it, and cite it [A4]`; `the paragraph citing A2 lacks "3": write it, in digits or words`; `A6 must be quoted verbatim, …`; `[A8] is not among the atoms to tell here: cite only the atoms given` |

   **A sentence that cites nothing is not written.** It is the sentence a writer adds to link two others, and the one that brings in what was not said: an article whose every sentence cites its atoms can be checked whole.

3. **Of the drafts that pass, the judge's reading decides.** Ledger B ranks them first, by its one warning, the talk's order (an atom told before one said earlier: worth a reader's look, not a repair); the first drawn on a tie. Where the tier asks for a judge (`judge_top`: two at `thorough` and `max`), the best of them are read by a model of **another family** than the writer, the infantry (Gemma, when Qwen writes), since a model favours its own kind:
   - **sentence by sentence, evidence first** (`prompts/judge`): for each sentence, the words of the atoms it cites, quoted; then what it says that they do not; then one of `none`, `adds`, `softens`, `intensifies`, `misattributes`, `contradicts`. The answer's fields are in that order, so the verdict is decoded after its evidence (`rr/pipeline/judging.hpp`);
   - **how they read, in both orders** (`prompts/flow`): each pair shown twice, swapped, without their markers, and a draft wins only when both orders prefer it (`reason::swapped()`): a judge that favours a side ties.

   The draft kept has the fewest issues the judge read, then the fewest warnings, then the fewest comparisons lost (`reason::select_lexicographic()`): what it says first, how it reads last, so fluency never buys back fidelity. What the judge reads in the draft kept is published as its reading (`section 1, "Weighing radium": the judge reads "…" as softening what its atoms say: …`). A judge that does not answer leaves the pick to Ledger B, and says so; so does one that does not hold a section's drafts (`section 1, "Weighing radium": the drafts are picked by Ledger B alone: they are more than the judge holds at once`), since the army refuses a squad with a mission no engine holds; a pod whose infantry is the commander's family has no judge, and says so once. Drafts to judge always come from the commander: an escalation draws one answer, and one draft is not judged. The judge never sends a draft back: repairs aimed at the sentences it reads wait for the eval to say they pay (P26).
4. **A section the commander does not hold is written in parts**, each the most paragraphs it holds from where the last ended (bisection, as for plans); each part's task says which paragraphs it writes, numbered as in the section, and the parts join in order.
5. **What no draft passed is told as its atoms say it**: each paragraph its atoms' words, each with its marker, words to quote that its words do not hold added in parentheses, with a finding that says why (`section 2, "The samples": it is told as its atoms say it: the models declined to write it`). Plain, but faithful: the floor of the article is its ledger.
6. **The article** is each section under its heading (`## …`), in order, a section without paragraphs left out. It is checked whole as Ledger B, the plan's duplicates excused, and what still fails is published. A video whose every sentence is filler has an empty article, and a finding says so.

**What the stage says while it works**: `write: the commander writes 3 sections, 4 drafts each` (`, in 4 parts` where it took more missions than sections), counting the sections it is asked for, and nothing when it is asked for none; then `write: the infantry judges the best 2 drafts of 3 sections`.

**The key.** Its fingerprint holds the tier's write settings, the prompts' labels and hashes (the writer's and the judge's two), the army and what the commander holds, and where the tier judges, what the judge holds. It samples: the job's salt is in its key. Without a pod its fingerprint is `Errc::needs_user`; an outline that does not cover its transcript, or a plan that does not hold its ledger as the outline cuts it, is an earlier stage's bug, `internal`.

**The oracle** writes by a rule a test can predict (`rr/oracle/answers.hpp`): each paragraph its atoms' words, in order, each followed by its marker; words to quote that the words do not hold, added in parentheses. Its draft passes Ledger B by construction, and is what the floor writes too. As a judge, it reads every sentence as saying what its atoms say, and prefers the shorter of two drafts, in either order; a test that wants an issue read, or a judge biased by position, changes its answer.

## The seams stage

**Where the sections meet.** The write stage writes each section apart, a closed world, so its joins fall as they may: a section that ends on the speaker's surprise before one that opens "She says…", pointing back to nobody; a word said on both sides of a heading; a section that starts over. This stage writes the two paragraphs at each seam again, together, each keeping its own atoms: only how things are said may change, never what (`rr/pipeline/seams_stage.hpp`).

1. **Where the seams are** is read from what the article's paragraphs cite, never from its headings: a paragraph belongs to the section of the plan whose atoms it cites, and a seam is where one section's paragraphs give way to the next's. A heading is a model's free text and could hold anything, even a line break; a citation is read by code.
2. **The commander rewrites each seam** (`prompts/seam`), without thinking, shown each paragraph's atoms as a writer is shown them, and the paragraphs as written. It changes most where the paragraphs meet: the first still begins, and the second still ends, much as written, since there they meet the rest of their sections.
3. **A rewrite is checked as a draft is** (`draft_problems()`, `rr/pipeline/drafting.hpp`, the write stage's checks): two paragraphs; each atom told in its own and in no other, since an atom told again across the seam would stand under another section's heading; every sentence citing; Ledger B held to their atoms. What fails is sent back once, and then asked once more with thinking, the ladder's next rung (`rr/agents/army.hpp`). A seam no rewrite passed stays as written, and it is said: `the seam between section 1, "Weighing radium", and section 2, "The samples", stays as written: no answer passed its checks (…)`.
4. **A rewrite is kept only where it is no worse.** At `thorough` (`[seams] mode = "rewrite"`), Ledger B decides: a rewrite with more warnings than the paragraphs as written is dropped. At `max` (`"recheck"`), the judge, of another family than the commander, reads the seam as written and as rewritten (`judge-seam-1-written`, `judge-seam-1-rewritten`). The rewrite is kept only where the judge reads no more issues in it, then Ledger B no more warnings: the order of the write stage's pick, a tie going to the rewrite. What the judge reads in a rewrite kept is published as its reading. A judge that cannot read a seam, one that does not hold it, and a pod without a judge of another family each leave the choice to Ledger B, and it is said. A rewrite the same as the paragraphs as written is not read at all.
5. **Seams that share a paragraph go in two waves.** A section of one paragraph ends one seam and starts the next, and two such seams rewritten at once would each undo the other. So the first wave holds every seam that shares no paragraph with an earlier seam of the first wave; the second holds the others, each shown its shared paragraph as the first wave left it. Most articles take one wave.
6. **The article** is checked whole again by Ledger B, the plan's duplicates excused, and what it finds that it did not find in the article as written is published: the write stage said the rest.

A rewrite stays the commander's: after a refusal, no other family writes it, so the judge never reads a rewrite of its own kind. At `fast` (`"none"`), the stage passes the article on as written and asks no model.

**What the stage says while it works**: `seams: the commander rewrites 2 seams`; then `seams: the infantry reads 1 seam, as written and as rewritten`, where the tier rechecks a rewrite that changed something; and `seams: the commander rewrites 1 more seam` for a second wave.

**The key.** At `none`, its tier's seams mode alone. Otherwise also the prompts (the seam's, and at `recheck` the judge's), the army, and what the commander and, at `recheck`, the judge hold. It samples: the job's salt is in its key. Without a pod, a tier that rewrites is `Errc::needs_user`. An article that cites an atom no section of its plan tells is the write stage's bug, `internal`.

**The oracle** rewrites a seam as written: an answer that passes every check and changes nothing. A test that wants a seam rewritten changes its answer.

## The commands

| Command | What |
|---|---|
| `rreport ingest SOURCE [--tier T] [--salt N]` | a new job for a web address or a caption file; `--tier` is checked before a job is made |
| `rreport resume JOB` | the job taken up again where it stopped; a done job says so. An eval annotation is refused: `rreport eval annotate` takes it up, with its own stages |
| `rreport jobs [--limit N]` | the library's jobs, newest first, each with its kind: `article` or `annotation` |
| `rreport jobs show JOB` | the job, and each stage: done, found done or failed, with its artifact or its error, and what it could not do |
| `rreport report JOB [--out FILE]` | the job's article as one web page, with its Fidelity Report, written to `FILE`, by default `reports/JOB.html` beside the library's database; it says where. A job not done is a page too, of what it has ([report.md](report.md)) |

Each stage says how it is doing on stderr. At the end, the command says what the stages could not do, then where the job stands and what to do next. The exit code is 0 for a job done and 1 for one that is not; a job stopped by a signal exits 128 + its number.

## What waits

- **How a seam reads.** A rewrite is kept for what it says, never for how it reads: the judge reads its fidelity, not its flow. Whether a flow comparison, as the write stage makes between drafts, would keep better seams is the eval's to say (P24, P26).
- **Repairs aimed at the sentences that fail.** A draft that fails is written again whole, with what failed; rewriting only its failing sentences, with their evidence, waits for the eval to say it pays (P26).
- **An article-wide plan.** Each section is planned alone, so an atom that repeats one of another section (a closing summary of the whole talk) is told again where it was said, and the writer may refer back. A pass over the whole article that finds such duplicates, and the article's title and lead, wait for the write stage and the eval (P20, P26).
- **Plans compared.** One plan is drawn a section, checked by code; whether drawing several and keeping the best pays is the eval's to say (P26).
- **More waves of samples** where the first disagree (a tier's `atomize.step`): refused until the eval shows adaptive waves pay (P28).
- **Thinking while describing**, and a brief from more than gists: the eval decides (P26).
- **The token estimator** is not yet calibrated from the call log: the default, which errs high, sizes every request.
- **A video read again**: a source is read once per library, so captions the creator fixed later are not seen. A way to read a source again, on purpose, will come with the library's commands.
- **A key that names the models costs a pod start.** A stage that asks models raises the army to make its key, even when its result is then found done. So ingesting again an automatic-captioned video whose transcript is done still starts the pod. Remembering the pod's engines from its last run would save that start, but a key made from a memory could name models the pod no longer serves. It waits until runs show the start matters; in practice, a job that reaches the pod's stages needs it anyway.
- **The end-to-end test through the real executables**, with oracle engines and `kill -9`, is P22.

## How the tests were validated

| Suite | What it holds |
|---|---|
| `tests/unit/pipeline/` (196) | the executor on scripted stages: found done, run again only where an input changed, a failure recorded and resumed, a stop, an artifact that breaks its schema, the salt, a result kept for one job, the cache key and the army's fingerprint; `run_job()`, and a video's length once its source is read; punctuation's windows and answer checks; the source stage on a fake yt-dlp and on files; the transcript stage on an army of oracles, with faults injected; sentence windows, planned against engines of many sizes, bisection against giving up one sentence at a time; what asking stages share; sections from chapters, pauses and agreement, long ones cut, briefs joined; the outline stage on a synthetic lecture: outlines, chapters, no pod, windows, a sentence too long, an engine too small, every check sent back and repaired, every failure a finding; a job's tier found through its context; readings voted on, grown and replaced, and what atoms hold of a sentence; the atomize stage on a synthetic lecture: every sentence accounted for, every rule sent back and repaired, votes and the commander's settling, sentences asked again, windows, ceilings, no pod, every failure a finding; atoms grouped in the order said; the plan stage on the same lecture: every section planned thinking, in a closed world, every rule sent back and repaired, duplicates that lose nothing, parts, what no answer planned told in order, ceilings, no pod; a prompt answered in prose; the write stage on the same lecture and its plan: every section written and every atom told, every rule sent back and repaired, the fewest warnings kept, duplicates left untold, parts, what no draft passed told as its atoms say it, an empty article, ceilings, no pod; the judge's readings checked and read; the drafts judged by another family, issues before warnings before how they read, both orders compared, a judge biased by position tied, one that declines or fails its checks never handed to the writer's family, ceilings, a pod without a judge |
| `tests/unit/library/` | jobs, and the stage store: one commit or none, an artifact kept once, a damaged one refused, findings per run |
| `tests/unit/hub/` | `JobArmy`: raised once, recorded engines replayed, what needs the user, the job's ceilings per hour of its video, asked when it is raised; the configured yt-dlp, and `[jobs]` |
| `tests/component/hub_cli_test.cpp` | `rreport ingest / resume / jobs / report` through the real executable: a caption file done without a pod, the outline's finding said, then every stage found done; no pod, a finding; a missing file; an unknown tier; a job's page where it stopped, the same wherever it is written, and an unknown job refused |

They pass under ASan and UBSan, and the pipeline, hub and library suites under TSan (the transcript stage's missions run on the army's threads). clang-tidy found what a reader might miss: an `optional` read without a check in `JobArmy` (now a `unique_ptr` that cannot be empty once raised), a `std::move` into a conditional that copied anyway, and fixtures with public state.

**The tests found three mistakes in the code before any mutant ran:**
1. The prompt rendered "by Science Hour.Its description": inja's `trim_blocks` also removes the spaces after a block tag. The template puts its blocks on lines of their own now ([prompts.md](prompts.md#writing-a-prompt)).
2. A test of the description's cut expected the newline kept, and the code dropped it: "…" now starts a line of its own.
3. A live video was `needs_user` in the source stage and `unavailable` in `choose_captions()` (P14). The stage's own check was a duplicate, and is gone: the job stops, to be resumed.

**Writing the mutants found gaps before they ran.** No test changed a stage's version, saw a found-done run recorded, titled a video that had no title, put a long pause before the shortest window, looked a relative path up in `PATH`, or asked the oracle without a script. Each has a test now. Two lines of the source stage changed nothing an artifact could show (clearing the offers and the live flag, which `to_json()` never writes), and are gone.

**The first run** killed 51 of the 53 pipeline mutants, the 10 hub mutants, the 2 rr_llm mutants, 3 of the oracle's and 1 of the 2 new prompt mutants:
- `a-job-is-not-saved-before-its-stages` survived. The test handed `run_job()` a job marked stopped that it had never saved, so the database said running either way. It saves the job stopped first now.
- `unknown-is-an-empty-text` was killed, but not by the test that claims it. The suite had crashed before reaching it, in a test that read the first stage of a run that had none. Every test now asserts how many stages a run reported before reading one.
- `a-prose-prompt-has-a-schema` survived: a prose prompt and a schema gone missing both answer `not_found`. The test now checks which one it is.
- `a-changed-file-is-kept` did not build (it left a parameter unused), and was written another way.

The final run kills all 70, each by the test that claims it: 53 in the pipeline (21 of the executor and the key, 6 of a job's run, 6 of the source, 9 of the transcript stage, 10 of windows and answers, 1 of the stage store's order), 10 in the hub, 2 of the prompt library's answer schema, 2 of a preset in a key, 3 of the oracle's answers.

**The outline stage (P17).** Its tests run it on a synthetic fifteen-minute lecture whose talk turns where the oracle starts a section, and on a 130-minute one for windows. What they found before any mutant ran:
1. **A commander too small for the tier's thinking budget would have cut the video into one-sentence sections.** At `max` the outline thinks 16,384 tokens; on an engine holding 16K or less no window fit, so every sentence was set aside, each starting a section of its own. Now thinking takes half of what the commander holds at most, and sentences set aside one after another are one stretch, cut where the speaker paused, with one finding. Two tests hold it: a 16K commander thinks 8,192 tokens, and a 2K one leaves the video as no pod would.
2. **A caption file now starts the pod.** The CLI test pinned "punctuated captions never start the pod"; with the outline in a job's chain, a configured pod reads every video. The test runs without a pod until the fake engines answer as the oracle does (P22).
3. The oracle titled a section with its sentence's full stop, so the video's gist read "…mean..": its titles drop the punctuation after their last word.
4. A test's fault rewrote `"meaning":null}` and never fired: `json::Json` sorts keys, so the oracle writes `{"meaning":null,"term":…}`. The fault now rewrites the text as written, and the test checks that its repair was asked.
5. A squad's missions run at once, and log their requests in any order: tests read them by mission.

clang-tidy found an optional read through an index it could not prove checked, a vector filled without a reserve, `std::iota` where `std::ranges::iota` serves, a variable that could be `const`, and a test's `uint32` product widened after the fact.

**Writing the mutants found tests missing,** each written: the naming of sentence ranges ("S4", "S4–S6") and of findings' openings; the lookup of the engine serving a role; a first chapter nearer the second sentence than the first; a brief listing a word three times naming it once; the prompts' hashes in the key; a speaker the video's description names, not the section.

**The mutants** (`tests/mutants/pipeline.toml`, 129). P15's 50 ran again, since the transcript stage's shared parts moved; 79 are new: 8 of what asking stages share, 14 of windows, 19 of sections and briefs, 38 of the outline stage. With them ran the outline artifact's 7 (`knowledge.toml`), the oracle's 5 new ones, and the eval's 42 in `annotate.cpp`, whose windows moved here. The first run killed 125 of the pipeline's 129:
- three did not build: each left a parameter, a function or a variable unused, which `-Werror` refuses, and they were written another way;
- `a-tie-goes-to-the-later-cut` survived. No test had two cuts exactly as near the middle that leave different parts; one does now (three sentences of ten seconds, cut for twenty-five);
- `every-outline-that-agrees-adds-a-cut` was killed before the test that claimed it could run. Its duplicate cuts reach `outline_of()` in the ceiling test, whose invariant aborts the process, as it should. It is claimed now by the first test that catches it.

The final run kills all 129, and the 7, 5 and 42, each by the test that claims it.

**The atomize stage (P18).** Its tests run it on a synthetic fifteen-minute lecture written for the oracle's rule: a greeting at either end, a disfluency, a number lent by the NRC, two hedges, one of the video's words, a thought that runs over two sentences, a name, a year and words to quote, a question, and Pierre named only near the end. The happy path reads it in two windows, three samples each; the faults are made on the first window's answers, as JSON, sentence by sentence. Readings, votes and coverage have their own tests (`readings_test.cpp`), as pure functions.

What the tests found before any mutant ran:
1. **The windows test assumed what bisection does not do.** An infantry holding one token less than the first window takes still reads the lecture in two windows: the first gives up a sentence or two, and the second takes them. The test now uses the 48 GB profile's own 4,096 tokens a slot, and the lecture is read in windows of 15, 12 and 3 sentences, to the same ledger.
2. A test counted the lecture's atoms from 0 and looked for S27's at index 22: it is 23, since S8 has none of its own.

**Writing the mutants found code that changed nothing**, now gone: a stretch's neighbours merged where `grown()` joins them anyway; fillers re-sorted when consecutive windows give them in order; a shortcut for a single reading, which wins every vote the same way; a sort of windows nothing read in order; a line for a sentence a reading made nothing of, which an accepted reading cannot do; the video's lexicon passed again to the final check, after every window was held to it. And ceilings noted in three places are noted in one (`fight()`, every squad's outcomes).

**It found a weakness, now fixed**: repair messages renamed every `A<digits>` they held, so a quoted sentence that says "A2" would have been renamed too. Only the atoms a finding names are renamed now, each as a whole word.

**And behaviour no test held, now tested**: a verdict sorted whatever order its atoms are written in; a stretch grown over two passes; labels replaced within a stretch; terms counting toward coverage; an answer's size, its least and its most; the seeds; what the stage says while it works; three sections named "A", "B" and "C"; what a reader made of a labelled sentence and of a thought that runs on; a sentence held exactly half, which is enough; an atom asked again that also cites a sentence not asked; a pod with no commander; an atom that also names an earlier sentence; `A1` beside `A18`; a name warning published and not sent back; a speaker only the brief names; and in the oracle, greeting words without a greeting, a thought that cannot run on past a gap, a closing quote, names split by a comma.

clang-tidy found `unsettled()` too complex to read (it is four named steps now), three optionals read through an index it could not prove checked (each an invariant, now asserted where it holds), and in the tests vectors filled without a reserve, parameters copied, an `int` product widened, a variable that could be `const`.

**The mutants** (`tests/mutants/pipeline.toml`, 227 now). 120 ran: 64 of the atomize stage, 30 of readings, 1 of a job's tier, 1 of the outline's chapters setting, and again the 14 of windows and the 10 of asking, since windows take their context as a parameter now, and asking holds `Problems`, with the two mutants the eval had for it. With them ran the oracle's 18 new ones (`oracle.toml`), one of the ledger's checks (`fidelity.toml`: the video's lexicon ignored), and the eval's 3 in `draft_problems()`, which notes its problems through `Problems` now. The first run killed 118 of the 120:
- `a-stretch-grows-in-one-pass` survived. The test's chain of two atoms had S3, the sentence both cite, out-voted as well, so one pass from S3 reached both atoms. S3 now wins its vote: only S5 is unsettled, and only a second pass reaches the first atom.
- `every-later-section-holds-the-window` did not build (it left a parameter unused), and was written another way.
- Six were killed, but not by the tests that claim them. Each made an atomize test read the first element of an empty list, undefined behaviour that ended the test binary before the readings tests ran. The tests read through `.at(0)` now, which throws: one test fails, and the run goes on.

The final run kills all 120, each by the test that claims it, and the 18, the 1 and the 3.

**The job's ceilings.** Their tests hold them per hour of video, a short video counted as a quarter hour and the tokens rounded up (`JobCeilings`); the length the site gives, else where timed captions end (a sound too), else words at 150 a minute; the length asked once, when the army is raised, and a length not found failing the raise; the army given the ceilings, not only told of them (`Army::ceilings()`); `[jobs]` read and refused at zero. Writing them found a search for the newest source run that a job cannot have: a job keeps one run a stage (`stage_runs` is keyed by job and stage), so it is a plain search now. The mutants (`hub.toml`, 12; `pipeline.toml`, 5) are all killed, each by the test that claims it.

**The plan stage (P19).** Its tests run it on the atomize stage's lecture and the ledger the oracle makes of it, now shared (`tests/unit/pipeline/lecture.hpp`): seven atoms in the first section, ten in the second, nine in the third. The faults are made on the commander's answers, as JSON. The plan's own rules have their tests (`plan_test.cpp`), and so do duplicates (`duplicates_test.cpp`) and grouping (`grouping_test.cpp`), as pure functions.

What the tests found before any mutant ran:
1. **A part's task ran into its section's line.** A probe of commander sizes, written to find one that splits a section, showed the task as "…from 5:00 to 9:55Its atoms are too many…": inja's `trim_blocks` takes the newline after a block tag, and the section's line ended with one (its gist's `{% endif %}`). Every block of the task has lines of its own now, the gist one of them.
2. The prompt check refused `gist` as both the video's (shared) and the section's (the order's): which one a template meant would depend on the order of rendering. The section's is `section_gist`.
3. Clang refused a field GCC let pass: the planner kept the outline and never read it.
4. A test expected an answer's room without the thinking's closing words, which the army counts too (`rr/agents/army.hpp`): the test says so now.

**Writing the mutants found behaviour no test held, now tested**: a heading of the commander's own (the oracle's is always the section's title); a later part's heading not replacing the first's; a commander that holds no atom even alone, which leaves one finding a section, not one an atom.

clang-tidy found `stretches_of()` too complex to read (its bisection is `most_held()` now), a result that could be ignored, a `push_back` that is an `emplace_back`, and three using-declarations the lecture's move left unused.

**The mutants** (`tests/mutants/pipeline.toml`, 279 now): 3 of grouping and 44 of the plan stage, and with them 18 of the plan (`knowledge.toml`), 12 of duplicates (`fidelity.toml`) and 6 of the oracle's plans (`oracle.toml`). The first run killed 81 of the 83:
- two did not build: each left a variable or a parameter unused, which `-Werror` refuses, and was written another way;
- `the-commander-thinks-past-half` was killed, but not by the test that claims it. That test checked the thinking of every mission asked, and a commander that thinks with all it holds holds no mission: there was none to check, and it passed. It asserts the three missions first now;
- `plan-is-not-taught` was claimed by a stage test, which the oracle's suite does not run; the oracle's own test of plans claims it now.

The final run kills all 83, each by the test that claims it.

**The write stage (P20a).** Its tests run it on the plan stage's lecture, its ledger, and the plan the oracle makes of them, now shared (`lecture_plan()`): three sections of three, four and three paragraphs. The oracle writes each paragraph as its atoms say it, so its draft passes Ledger B, and each fault breaks one rule of it, as text. The scope of a text and the duplicates it excuses have their own tests (`tests/unit/fidelity/article_test.cpp`), and so does a prompt answered in prose (`asking_test.cpp`).

What the tests found before any mutant ran:
1. **The first prompt answered in prose could not be asked.** `stage_prompt()` asked every prompt for its answer's schema, and `write` has none: "prompt 'write' is answered in prose". A prompt without a schema is asked without one now.
2. **An empty article broke its own schema.** A video whose every sentence is filler has nothing to retell, and the article's schema asked for at least one character. It allows an empty article now, and a finding says why it is empty.
3. **The prompt's example showed an atom another way than the stage does.** The stage ends an atom's words to quote with a full stop (`To quote: "…".`), and the example in the system part did not. A model is now shown exactly the shape it is given.
4. **Both stages that ask the commander miscounted what they asked.** The plan stage and the write stage counted every section with atoms or paragraphs, so a section none of whose paragraphs the commander holds was said to be written, and a video with nothing to tell said `the commander writes 0 sections`. They count the sections the commander is asked about, and say nothing when it is asked about none.
5. clang-tidy found a nested conditional (the fallback's wording is `told_plainly()` now) and a test's `int` product widened after the fact. It crashed once, with SIGBUS, reading a file rewritten while it ran; it was run again.

**Writing the mutants found behaviour no test held, now tested**: an answer's least and most room; a long sentence quoted in a message, cut before a character, never inside one; a part's paragraphs numbered as in the section when an atom is sent back; a register label; a list of two terms; a question's modality said once; the video's gist; the seeds; the first of the drafts with fewest warnings, when two that tie differ; a warning no draft avoids, published; a part the models decline; a commander that holds no paragraph; an outline that does not cover its transcript; the key's tier, the write stage's own; no word on where atoms are when the paragraphs were miscounted; and in the oracle, an atom with terms alone, and a block with no paragraph.

**The mutants** (`tests/mutants/pipeline.toml`, 358 now): 1 of a prompt answered in prose, 2 of what the plan stage says while it works, and 76 of the write stage; with them 11 of the scope of a text (`fidelity.toml`), 1 of the empty article (`knowledge.toml`) and 9 of the oracle's articles (`oracle.toml`). The first run killed 96 of the 100, each by the test that claims it. Four did not build, each refused by `-Werror` for a reason a mutant must avoid: a scope left unused, a value compared with itself, an unsigned number compared with zero, a conditional whose branches were the same. Each was written another way, and the final run kills all 100, each by the test that claims it.

**The judge (P20b).** Its tests run the write stage as before, the oracle judging too: it reads every sentence as saying what its atoms say, and prefers the shorter of two drafts, so a test changes its answers to make it read an issue, or favour a position. The readings' own rules have their tests (`judging_test.cpp`).

What the tests found before any mutant ran:
1. **The judge's family was checked twice**: against the commander, and against each draft's writer. The second can never matter: drafts to judge, two or more, are the commander's, since an escalation draws one answer. It is gone, and the reason is written where the first check is.
2. A test of position bias faked the order that already agreed with the drafts' lengths, and changed nothing: the fault belongs to the order where the longer draft is shown first.
3. The prompt check wants an example of two orders or more, to show the system part is the same for each: both new prompts had one.
4. A ceiling reached while judging is tested with the judge's missions one at a time, since the army checks a ceiling as a mission starts: the first reading spends past it, and every mission after stops.
5. clang-tidy found the issues' table without designated initializers, and a `string_view`'s data passed without its size.

**Writing the mutants found behaviour no test held, now tested**: the ranking that chooses which drafts are judged; issues outranking the talk's order; three drafts compared pair by pair; a judge whose readings fail its checks, never handed to the writer's family; a heading, which is no sentence to judge. **The first run found more**: a single draft at a tier that asks for a judge; the first-ranked draft winning its comparison; and three drafts in two orders of length, since in one order the shortest wins whichever comparison is misread; in the oracle, a line that does not open with its number.

**The mutants** (`tests/mutants/pipeline.toml`, 399 now): 42 of the judge (11 of its readings, 31 of the stage's use of them), and the write stage's 75 again, three of them rewritten for code the judge moved; with them the oracle's 7 of judging and 11 of articles (`oracle.toml`). The first run killed 129 of the 135: five survived, each a gap above; one did not build (an unsigned number compared with zero); and one was then killed by the new test of a single draft, which claims it now. The final run kills all 135, each by the test that claims it.

**The seams (P20c).** Its tests run the seams stage on the write stage's lecture article: three sections, so two seams. The oracle rewrites a seam as written, so a test that wants a rewrite changes the commander's answer, as text, and one that wants an issue read changes the judge's. Where the seams are and the two waves are tested on articles a test builds: sections of one paragraph, alone and in a chain. What two stages now share (`drafting.cpp`, `judging.cpp`) moved with its tests' mutants, and is held by the write stage's tests as before.

What the tests found before any mutant ran:
1. **The write stage's judge could be refused whole.** The army refuses a squad with a mission no engine holds, so a section's drafts the judge did not hold would have failed every section's reading. Building the seams' judge, which asks the same, showed it: the write stage now asks the judge only about the sections it holds, picks the others by Ledger B alone, with a finding, and keeps what the judge holds in its key.
2. **A draft could tell an atom twice.** The write stage sent back an atom told in another paragraph than its own, but not one told in its own and again in another. Across a seam, the second telling would stand under the other section's heading. Both stages send it back now (`A1 is told again in paragraph 2: tell it in paragraph 1 only`).
3. A test expected the warning of the wrong atom, A9 where the rewrite moves A10: the expectation was wrong, not the code.
4. clang-tidy found messages with quotes it reads better as raw strings; the function that smooths an article too complex to read at once, now three (`rewritten()`, `read_again()`, `put_in_place()`); optionals read before a check, now bound first; a `const` that kept a value from being moved; a `using` nothing used.

**A review of the code found one gap more**: where the seams are was read from each block's first marker alone, so a block that cites, after its first, an atom no section of the plan tells would have been placed by its first and not refused. Every marker is checked now, and a test cites `[A7][A27]`.

**The mutants** (`tests/mutants/pipeline.toml`, 446 now): 82 of P20c, 47 new (40 of the seams stage, 3 of telling an atom again, 4 of the write stage's judge and what it holds) and 35 run again (the 34 that moved with the code two stages share, and the write stage's key, which changed); with them the oracle's 3 of seams (`oracle.toml`). The first run killed 80 of the 81 then written, each by the test that claims it, and the oracle's 3. One survived: the article's last check published Ledger B's findings whole, not only the new ones, and no test noticed, because the only old warning in its article was one the rewrite removed. The test now also keeps an old warning in a paragraph no seam touches. The final run kills it, the review's new mutant and the two of where the seams are, whose code the review changed.

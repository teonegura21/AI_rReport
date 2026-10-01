# The pipeline: jobs, stages, and the stage cache

A **job** takes one video through the **stages**: today `source` and `transcript`, then `outline`, `atomize`, `plan` and `write` as they land (P17–P20). The eval's annotation jobs run `source`, `transcript` and `annotate` ([eval.md](eval.md#silver-drafted-by-models)). Each stage turns the artifacts of the stages before it into one artifact of its own, an immutable JSON document that follows its schema (ADR-0004). This note is about what runs them, `rr_pipeline`, and the first two stages.

| Piece | Where | What |
|---|---|---|
| `Stage` | `rr/pipeline/stage.hpp` | What a stage reads and makes, its version, its schema, its fingerprint; the cache key |
| `Executor` | `rr/pipeline/executor.hpp` | Each stage of a job found done or run, checked, committed |
| `run_job()` | `rr/pipeline/job_run.hpp` | A job taken through the executor and saved: running, done, stopped, needs_user, failed |
| `Jobs`, `StageStore` | `rr/library/` | The jobs table, each job of a kind: an article, or the eval's annotation (`migrations/0005_job_kind.sql`); artifacts, the stage cache and each job's runs (`migrations/0004_pipeline.sql`) |
| `JobArmy` | `rr/hub/job_army.hpp` | A job's army, raised the first time a stage needs a model |
| `SourceStage` | `rr/pipeline/source.hpp` | A video (through yt-dlp) or a caption file, read once |
| `TranscriptStage` | `rr/pipeline/transcript_stage.hpp` | Captions into numbered sentences; punctuated by the infantry when they are not |

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

A pod costs money from the minute it starts. So a stage is given **the means to raise the army** (`StageContext::army`, a function), not an army, and a job whose stages need no model never starts the pod. A video with its creator's captions, today, never does.

`JobArmy::raise()` raises it once per job:

1. **The pod's engines.** In live and record mode they come from the runtime's health report, once every engine is ready. A pod RunPod manages is started if it is stopped, as `rreport pod start` would. In replay mode no runtime is called: the engines come from `engines.json`, beside the recordings, which record mode writes as it raises its army. A replay therefore raises the same army as the one recorded.
2. **The stack every call goes through** (`ModelStack`: the runtime and nowhere else, retries, accounting into `llm_calls`, recording), and over it **the job's memo**, keyed by each model's file checksum: a resumed job asks again none of the calls it already made.
3. **The army**, with the job's time before this run counted against its wall-clock ceiling, logging each mission in `missions`.

A failure is returned to the stage that asked, and the next stage that asks tries again: a pod that did not come up may come up later. A stage that needs the army also raises it in its fingerprint, since its key names the models that will answer.

Without a runtime configured, and outside replay mode, the CLI gives the stages no means to raise an army at all. A stage that needs a model then does what it can without one, and says so in a finding.

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

## The commands

| Command | What |
|---|---|
| `rreport ingest SOURCE [--tier T] [--salt N]` | a new job for a web address or a caption file; `--tier` is checked before a job is made |
| `rreport resume JOB` | the job taken up again where it stopped; a done job says so. An eval annotation is refused: `rreport eval annotate` takes it up, with its own stages |
| `rreport jobs [--limit N]` | the library's jobs, newest first, each with its kind: `article` or `annotation` |
| `rreport jobs show JOB` | the job, and each stage: done, found done or failed, with its artifact or its error, and what it could not do |

Each stage says how it is doing on stderr. At the end, the command says what the stages could not do, then where the job stands and what to do next. The exit code is 0 for a job done and 1 for one that is not; a job stopped by a signal exits 128 + its number.

## What waits

- **Job ceilings per video-hour** (ADR-0018) need the video's duration, which the source stage now knows. They arrive with the first stages that spend much (P17). Until then a job has no ceiling, and punctuation is bounded per mission: its answer's size, its repairs, the ladder.
- **The token estimator** is not yet calibrated from the call log: the default, which errs high, sizes every request.
- **A video read again**: a source is read once per library, so captions the creator fixed later are not seen. A way to read a source again, on purpose, will come with the library's commands.
- **A key that names the models costs a pod start.** A stage that asks models raises the army to make its key, even when its result is then found done. So ingesting again an automatic-captioned video whose transcript is done still starts the pod. Remembering the pod's engines from its last run would save that start, but a key made from a memory could name models the pod no longer serves. It waits until runs show the start matters; in practice, a job that reaches the pod's stages needs it anyway.
- **The end-to-end test through the real executables**, with oracle engines and `kill -9`, is P22.

## How the tests were validated

| Suite | What it holds |
|---|---|
| `tests/unit/pipeline/` (42) | the executor on scripted stages: found done, run again only where an input changed, a failure recorded and resumed, a stop, an artifact that breaks its schema, the salt, a result kept for one job, the cache key and the army's fingerprint; `run_job()`; windows and answer checks; the source stage on a fake yt-dlp and on files; the transcript stage on an army of oracles, with faults injected |
| `tests/unit/library/` | jobs, and the stage store: one commit or none, an artifact kept once, a damaged one refused, findings per run |
| `tests/unit/hub/` | `JobArmy`: raised once, recorded engines replayed, what needs the user; the configured yt-dlp |
| `tests/component/hub_cli_test.cpp` | `rreport ingest / resume / jobs` through the real executable: a caption file done without starting the pod, then found done; no pod, a finding; a missing file; an unknown tier |

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

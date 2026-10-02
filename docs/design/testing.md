# Testing: what is proven where

rReport's promise is an article that is faithful to the video: every sentence accounted for, every number right, every quote verbatim. Most of that cannot be checked by looking at an article. So the system is built to be checked in layers, each proving what the one below cannot, and each test is itself tested: rules are removed on purpose, and a test must fail (mutation testing).

## Layers

| Layer | What it proves | Where | When |
|---|---|---|---|
| L0 static | The shipped prompts and tiers render, are versioned, and are the source tree's; mutation specs still find their code | `tests/unit/prompt`, `tests/unit/reason` (tiers), `scripts/test_mutate.py` | every push |
| L1 unit | Each module's rules, one by one, with exact expectations | `tests/unit/<module>/`: one GoogleTest binary per module, under GCC and under Clang with ASan and UBSan | every push |
| L1 fuzz | The invariants of code that reads untrusted input, on any bytes: no crash, no out-of-bounds read, nothing lost | `tests/fuzz/`: each corpus replayed by CTest under every build; libFuzzer searching (`fuzz` preset) | every push: the replay everywhere, two minutes of search |
| L2 component | The real executables together: the runtime with fake engines, the hub's CLI against it; and every threaded module under ThreadSanitizer | `tests/component/`, the `tsan` job | every push |
| L3 pipeline | Whole jobs without a GPU: every stage, check, repair and escalation, on synthetic videos whose right answers are known, with faults injected | the oracle (below), stage by stage; and whole jobs through the real executables, killed and resumed ([below](#end-to-end-killed-and-resumed)) | every push |
| L4 live | That the fake engine tells the truth: the runtime against a real llama-server, built for the CPU at the pinned commit, with a tiny model | `tests/live/`, `.github/workflows/nightly.yml` ([runtime.md](runtime.md#the-real-engine-nightly-l4)) | nightly, and when the suite changes |
| L5 eval | Quality, on the pod, with real models and statistics (ADR-0019) | runbooks R3–R6 | before a change of model, prompt or tier is kept |

## Stand-ins

| Stand-in | Stands in for | Where | Used by |
|---|---|---|---|
| Fake engine | llama-server, as a process: streaming, slots and their context, `/props`, the prompt cache, failures on cue; with `--fake-oracle`, the oracle's answers, and hangs on cue | `tests/fake_engine/` | the supervisor's and the hub's component tests, the end-to-end test |
| `FakeRunpod` | RunPod's pod API, in process | `tests/fakes/` | the runtime's self-stop, `rreport pod` |
| `ScriptedProvider` | A model that answers a script, in order | `tests/unit/llm/` | retries, accounting, record and replay, structured answers |
| `KeyedProvider` | A model that answers by what a request asks; it can hold a model's calls, to count how many run at once | `tests/unit/agents/` | the army: squads arrive in any order |
| `Oracle` | A model that answers every mission right | `tests/oracle/` | L3 |
| `FaultyProvider` | The mistakes models make, where a test says | `tests/oracle/` | L3 |
| `RequestLog` | A provider that keeps every request it passes on, for a test to read what a stage asked | `tests/oracle/` | the stages' tests |
| `FakeClock` | Time | `rr/core/clock.hpp` | timeouts, ceilings, durations, without sleeping |
| `TempDir` | A private scratch directory | `tests/testkit/` | databases, blobs, model files |

**A stand-in is only as good as its faithfulness.** That is why L4 exists: each claim the fake engine makes is checked against a real llama-server every night. Examples are the context per slot, schema field order, per-choice usage, the thinking budget, the prompt cache, the failures of an overflowing shared pool, and each token's probability covering an answer byte for byte (P28).

## The oracle and fault injection

**The rule every fault is tested against:** it ends repaired, or published as a finding; never silently in the article.

- **`Oracle`** answers each mission kind with what a stage teaches it (`teach("atomize", …)`). It computes the right answer from the request alone: the sentences in the packet, the atoms, the plan. It is deterministic, so a pipeline run on it can be compared with goldens. A kind it was not taught is `Errc::unsupported`: it never guesses. Each stage teaches it as the stage lands (plan P17–P20); `teach_stages()` teaches every kind so far (`rr/oracle/answers.hpp`):
  - `punctuate`, from the script a synthetic video was read from ([pipeline.md](pipeline.md#the-prompt));
  - `annotate` and `annotate_more`, the eval's silver drafts, which have no single right answer: the oracle answers by a rule a test can predict and that passes every check. Every sentence of five words or more is a fact, quoted whole, core when it holds a digit; the other family adds every sentence of three words or more that its notes do not cite ([eval.md](eval.md#decisions));
  - `outline`, `describe` and `brief`, by rules of the same kind: a section starts at S1 and where a sentence opens with "Now," or "Next,"; a speaker for each "I'm"; a word for each acronym ([pipeline.md](pipeline.md#the-key-and-the-prompts));
  - `atomize`, in all three of its modes: filler words alone are a disfluency and greeting words a greeting; any other sentence is one atom, the sentence itself, running on into the next when it ends without a full stop; a digit makes a number, "?" a question, "maybe" a hedge; capitalized runs are names, acronyms terms, the words between double quotes a must-quote. Asked again, it adds the atom a sentence's atoms miss ([pipeline.md](pipeline.md#the-atomize-stage));
  - `plan`: the atoms shown, three a paragraph in order, each paragraph's purpose naming its atoms; an atom shown exactly as an earlier one is declared its duplicate; the section's title as its heading ([pipeline.md](pipeline.md#the-plan-stage)).
  - `write`, in prose: each paragraph shown, its atoms' words in order, each followed by its marker; words to quote that an atom's words do not hold, added in parentheses. A draft so written passes Ledger B, so a test sees exactly what a fault breaks ([pipeline.md](pipeline.md#the-write-stage)).
  - `judge` and `flow`, the write stage's judge: every sentence says what its atoms say, its evidence the cited atoms' words; of two drafts, the shorter reads better, and two of a length tie: a rule that holds in either order, so swapping never turns its preference into a tie.
  - `seam`, the seams stage's: the two paragraphs as written, which pass every check a rewrite is held to; a test that wants a seam rewritten changes the answer ([pipeline.md](pipeline.md#the-seams-stage)).
- **The stages that read atoms share one synthetic video** (`tests/unit/pipeline/lecture.hpp`): a fifteen-minute lecture written for the oracle's rules, its source, its outline, and the ledger and plan the oracle makes of it. The atomize stage's tests write that ledger and the plan stage's that plan; the write stage's read both.
- **The stages' tests share one rig** (`tests/unit/pipeline/oracle_army.hpp`): an army whose models are the oracle, taught every stage's missions, with the faults a test injects, and every request kept for the test to read. Its engines are sized per test (`Sizes`: what each role holds, its slots, how long it thinks), and `variant_tiers.hpp` gives a test a tier of its own (`StageContext::tiers`) for the settings no shipped tier has: a tier that takes no chapters, one that asks for waves, one that leaves contested sentences to the vote.
- **`FaultyProvider`** wraps any provider and makes the plan's faults on the calls a test picks:

  | Fault | What it does | What must follow |
  |---|---|---|
  | `refusal` | "I'm sorry, but I can't help with that." | another family answers, or a finding |
  | `truncation` | the first half, ended by the token limit (never inside a UTF-8 character) | a repair |
  | `broken_json` | the last closing brace or bracket gone | a repair |
  | `out_while_thinking` | no answer: the limit came while thinking | asked again without thinking |
  | `wrong_number` | the first number, plus one | the check catches it; a repair |
  | `bad_marker` | the first `[A#]` cites `A999` | the ledger catches it; a repair |
  | `dropped_reference` | the first `"S#"` left out (a JSON list stays a list) | the closed world catches it; a repair |
  | `server_error` | the call fails (`unavailable`, …) | an error, not a finding: the job stops, to resume |
  | `edit` | a stage's own: softening a quote, misattributing | the stage's check |

  `When` picks the calls: by mission, mission kind, model, thinking or not, and how many calls of each mission it hits. The default is the first call, so the repair succeeds; `calls = 0` hits every call, so the ladder's top is reached. Where two faults match a call, the first listed is made. `injected()` lists every fault made, and whether it found something to change.
- **The tests now** run each fault through the army against the oracle (P10), and check its end: repaired, rescued by another family, retried without thinking, a finding, or an error. The faults that change what a model writes are each caught by a fidelity check (P13, [fidelity.md](fidelity.md#faults-caught)).
- **The oracle answers on the wire too** (`rr_fake_engine --fake-oracle`), for the end-to-end test below. A call on the wire does not carry its mission kind: the tag is never sent to a model. So the fake engine knows a mission by its answer schema's description, which names its prompt (`… (prompts/atomize)`); a mission answered in prose (`write`, `seam`) sends no schema, and is known by its task's first line. A mission it does not know is refused with 400: the oracle never guesses.

## End to end, killed and resumed

`tests/component/end_to_end_test.cpp` takes a job through every stage the way an operator does: the real `rreport` against a real `rreport-runtime`, whose two fake engines answer as the oracle does. The commander's model is named `qwen-commander` and the infantry's `gemma-infantry`, so the hub gives them their families' sampling presets, and the judge is of another family than the writer. The video is the stages' synthetic lecture (`tests/unit/pipeline/lecture.hpp`) as a caption file, punctuated, so no model punctuates it.

1. **A clean run** at `max`: every stage done, both engines asked; the page's Fidelity Report at 100% of sentences accounted for and of atoms told, with no invalid marker; the job's memo forgotten once it is done.
2. **Six kills.** Each starts a new library, and engines one of which answers its first N calls, then hangs every later one (`--fake-hang-after`): each engine's first call and its last, and two drawn from a seeded generator. Once a call hangs, the hub is killed with SIGKILL, and `rreport resume` takes the job up against engines that do not hang.
3. **What is checked** is ADR-0020's guarantee, as a number: on resume, each engine answers exactly the calls of the clean run that the job's memo did not keep, `asked again = clean − kept`; and the seams stage's article is the clean run's, by its hash, so byte for byte.

**The eval runs on the same engines** (P23): `rreport eval run` takes the lecture's snapshot through every stage, twice, a replicate each; `score` scores both articles, and `export` writes their rows, as CSV and as JSON. The same run asked for again asks the engines nothing, and a label asked for with another number of replicates is refused. Then `judge` (P24) reads both articles with a panel frozen for the files those engines serve, against a reviewed gold of one fact: both judges asked, the export showing the code's scores and the judges' side by side (`code@1+judges@e2e-v1`), and the same judgement asked again asking nothing. And two runs of one pipeline are compared and gated (P25), on the lecture under two names, since an interval needs two videos: the second run asks the engines nothing, every stage run shared under the same salts, so `compare` shows no difference and `gate` passes; a third run, under other salts, asks the engines again and holds the gate's margins as an A/A run. The two runs' articles are then read side by side for flow (P25b), each part by both judges in both orders: every part a tie, and the pairs, read again, asking nothing. And a sweep (P26) of one variant, `max` with one draft instead of four: its writer asks the engines again, every stage before it is the baseline's own, the sweep asked again asks nothing, and `eval report` draws both runs. Last, labels (P27): the baseline judged, `eval label` draws what the judges read, a person who reads as the oracle does labels it, and `eval calibrate` says why one class a kind cannot pass a panel.

**Why the engines hang, rather than the kill falling at a random moment.** The oracle answers at once, so a kill timed by the clock would often find the job done, and test nothing. An engine that hangs holds the job at any point of it. A call may still be in flight on the other engine when the kill lands: its answer was not kept, so it is asked again, and the formula counts it. That is why the check is not the plan's "completions equal a clean run's" taken literally: a run killed while answers arrive repeats those whose memo rows were not yet committed, and only those. The test also fails if no kill left an answer kept: kills that all fell before the first one would prove nothing.

**How the test was validated.** Its mutants (`tests/mutants/end_to_end.toml`) break what a resume relies on, in the code the executables run: a kept answer asked again (the memo never read), no answer kept (never written), a job done that keeps its memo, and an engine that never hangs. Each makes the test fail. Its first runs found two mistakes of its own, and one of the fake engine's: it expected the lecture's 30 sentences, where its caption file has 29, since the segmenter rightly joins a cue that runs on into the next; it read the engines' counts before the runtime had started them, so an engine not yet listening counts as having no hung call now; and the fake engine compared an iterator of a schema with one of its parent, which nlohmann's JSON refuses. The test takes about 40 seconds.

## Fuzzing

A unit test checks the inputs its author thought of. A fuzz target checks *invariants* on whatever bytes arrive: a captions file in another encoding, a model's broken JSON. It is `LLVMFuzzerTestOneInput()`, which aborts when one breaks (`tests/fuzz/<name>_fuzz.cpp`), and it is built twice (`cmake/RrFuzzing.cmake`):
- **the replay**, `rr_<name>_fuzz_replay`, runs the committed corpus `tests/fuzz/corpus/<name>/` under CTest (`fuzz.<name>`), in every build-test job: GCC, ASan and UBSan, TSan;
- **the search**, `rr_<name>_fuzzer`, is linked with libFuzzer when `RR_FUZZ=ON` (preset `fuzz`: Clang, ASan, UBSan, every target instrumented for coverage). The `fuzz` CI job runs each for two minutes and uploads what broke an invariant.

An input that broke an invariant joins the corpus once fixed, as `regression-*.bin`, and gets a named unit test, so the rule is stated where readers look.

**What is fuzzed:** `rr_text` ([text.md](text.md#fuzzing)), whose fuzzer found its first bug within a second: a word of accents alone folded to an empty term. And the article's `[A#]` marker parser ([knowledge.md](knowledge.md#the-articles-markers)), which ADR-0006 calls correctness- and security-critical. And caption files of every format, read, cut into sentences and projected ([transcript.md](transcript.md#how-the-tests-were-validated)). Within seconds, that target found a NUL byte kept inside a word, and found `rr_json`'s parser throwing on a number too large for a double, which its contract forbids. And where each value of a JSON text stands (`json::spans()`, P28), differentially: on any bytes it refuses exactly what `parse()` refuses, with its message, and on JSON each span lies within its parent and parses to the value `parse()` puts at its pointer, one span a value. Its corpus's first replay found a key written twice giving spans to a value `parse()` drops; the earlier value and everything in it have none now. Its 7 mutants (`json.toml`) are each killed by the test that claims it.

## Mutation testing

**Why.** A green suite proves the code passes its tests; it does not prove the tests can fail. So each rule is removed on purpose, a mutant: a check skipped, a bound moved, an order swapped. The test that claims the rule must then fail. Writing mutants also shows which rules no test checks at all, before anything runs, as each design note's "How the tests were validated" records.

**How.** `scripts/mutate.py tests/mutants/<module>.toml [names…]` applies each mutant in turn, rebuilds the suite (preset `dev`), runs it, and restores the sources, checking them byte for byte at the end. The verdicts:
- **KILLED**: the named test failed;
- **KILLED (other tests)**: guarded, by another test than the one named;
- **KILLED (timeout)**: the suite hung, which is guarded but not cleanly. Make the test fail instead: tests that hold calls release them on every path;
- **SURVIVED**: no test guards the rule. Write the test;
- **BUILD FAILED**: write the mutant another way;
- **PATTERN NOT FOUND** or **AMBIGUOUS**: the edit's text must be in its file exactly once.

**The specs follow the code.** `scripts/test_mutate.py` runs on every push, and checks that every spec still finds each edit's text exactly once. A refactor that moves a guarded line fails CI until its mutant is updated, so the record never silently drifts from the code it guards.

**What has specs:** `pipeline` (485 mutants, the stage store's commit, a job's tier variant, atomize's waves and how sure a reading was, and the fusion of the commander's drafts too), `text` (112), `eval` (324: P24's panel and judgement, P25's statistics, gate and flow, P26's sweeps and page, P27's labels and calibration too), `oracle` (80), `knowledge` (69), `agents` (66), `fidelity` (55), `transcript` (53), `reason` (72, tier variants, a lone sample's confidence and `fuse` too), `prompt` (27), `media` (24), `hub` (22: the job army and its ceilings, the doctor's tools, the media settings), `report` (23: the page, a job read back, an eval job refused one), `end_to_end` (14: what a resume relies on, an A/A run's salts, pairs read, a sweep's variants run and summed up, a calibration's verdict, token probabilities through the runtime, a fusing variant and the fake engine's fusions), `net` (6, the server's stop), `library` (12: a job's kind, migrations 0007 and 0009 making `jobs` anew, migration 0008's first replicate), `hub_cli` (11: `eval gate`'s exit codes, `eval compare --flow`'s refusals and notes, a pairing never resumed, the eval's page and an ingest's variant, a person's verdicts never written over, a calibration refused without its labeller), `db` (5: migrations run with foreign keys off, and checked), `llm` (20: token probabilities asked, placed and kept, P28; a drafted token never weighed, P29), `json` (8: where each value of a text stands, P28), `supervisor` (4: the shipped catalog's MTP twins, P29), and a few in `core`, where later phases changed its code. The modules built before P7 were mutation-tested the same way. Their results are recorded in their design notes, and porting the rest of their specs into `tests/mutants/` is follow-up work.

**When to run:** before committing a change to a module's rules, and before a milestone. Never with another build reading the same sources, such as clang-tidy or a sanitizer build: it would compile a mutant.

## Running it yourself

```bash
ctest --preset dev                                   # L0–L2
ctest --preset dev -L agents                         # one module
build/dev/tests/unit/agents/rr_agents_tests --gtest_repeat=40 --gtest_shuffle   # flakiness
ctest --preset dev -L fuzz                           # the fuzz corpora, replayed
python3 -m unittest discover -s scripts -p 'test_*.py'                          # the scripts, and the specs' drift
scripts/mutate.py tests/mutants/agents.toml          # mutation testing (about 30 minutes)
```

The nightly L4 job runs by hand from GitHub (Actions → nightly → Run workflow), or locally ([runtime.md](runtime.md#the-real-engine-nightly-l4)).

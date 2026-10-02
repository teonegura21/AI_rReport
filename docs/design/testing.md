# Testing: what is proven where

rReport's promise is an article that is faithful to the video: every sentence accounted for, every number right, every quote verbatim. Most of that cannot be checked by looking at an article. So the system is built to be checked in layers, each proving what the one below cannot, and each test is itself tested: rules are removed on purpose, and a test must fail (mutation testing).

## Layers

| Layer | What it proves | Where | When |
|---|---|---|---|
| L0 static | The shipped prompts and tiers render, are versioned, and are the source tree's; mutation specs still find their code | `tests/unit/prompt`, `tests/unit/reason` (tiers), `scripts/test_mutate.py` | every push |
| L1 unit | Each module's rules, one by one, with exact expectations | `tests/unit/<module>/`: one GoogleTest binary per module, under GCC and under Clang with ASan and UBSan | every push |
| L1 fuzz | The invariants of code that reads untrusted input, on any bytes: no crash, no out-of-bounds read, nothing lost | `tests/fuzz/`: each corpus replayed by CTest under every build; libFuzzer searching (`fuzz` preset) | every push: the replay everywhere, two minutes of search |
| L2 component | The real executables together: the runtime with fake engines, the hub's CLI against it; and every threaded module under ThreadSanitizer | `tests/component/`, the `tsan` job | every push |
| L3 pipeline | Whole jobs without a GPU: every stage, check, repair and escalation, on synthetic videos whose right answers are known, with faults injected | the oracle (below); stage by stage from plan P17, end to end with `kill -9` and resume from P22 | every push, as stages land |
| L4 live | That the fake engine tells the truth: the runtime against a real llama-server, built for the CPU at the pinned commit, with a tiny model | `tests/live/`, `.github/workflows/nightly.yml` ([runtime.md](runtime.md#the-real-engine-nightly-l4)) | nightly, and when the suite changes |
| L5 eval | Quality, on the pod, with real models and statistics (ADR-0019) | runbooks R3–R6 | before a change of model, prompt or tier is kept |

## Stand-ins

| Stand-in | Stands in for | Where | Used by |
|---|---|---|---|
| Fake engine | llama-server, as a process: streaming, slots and their context, `/props`, the prompt cache, failures on cue | `tests/fake_engine/` | the supervisor's and the hub's component tests |
| `FakeRunpod` | RunPod's pod API, in process | `tests/fakes/` | the runtime's self-stop, `rreport pod` |
| `ScriptedProvider` | A model that answers a script, in order | `tests/unit/llm/` | retries, accounting, record and replay, structured answers |
| `KeyedProvider` | A model that answers by what a request asks; it can hold a model's calls, to count how many run at once | `tests/unit/agents/` | the army: squads arrive in any order |
| `Oracle` | A model that answers every mission right | `tests/oracle/` | L3 |
| `FaultyProvider` | The mistakes models make, where a test says | `tests/oracle/` | L3 |
| `RequestLog` | A provider that keeps every request it passes on, for a test to read what a stage asked | `tests/oracle/` | the stages' tests |
| `FakeClock` | Time | `rr/core/clock.hpp` | timeouts, ceilings, durations, without sleeping |
| `TempDir` | A private scratch directory | `tests/testkit/` | databases, blobs, model files |

**A stand-in is only as good as its faithfulness.** That is why L4 exists: each claim the fake engine makes is checked against a real llama-server every night. Examples are the context per slot, schema field order, per-choice usage, the thinking budget, the prompt cache, and the failures of an overflowing shared pool.

## The oracle and fault injection

**The rule every fault is tested against:** it ends repaired, or published as a finding; never silently in the article.

- **`Oracle`** answers each mission kind with what a stage teaches it (`teach("atomize", …)`). It computes the right answer from the request alone: the sentences in the packet, the atoms, the plan. It is deterministic, so a pipeline run on it can be compared with goldens. A kind it was not taught is `Errc::unsupported`: it never guesses. Each stage teaches it as the stage lands (plan P17–P20); `teach_stages()` teaches every kind so far (`rr/oracle/answers.hpp`):
  - `punctuate`, from the script a synthetic video was read from ([pipeline.md](pipeline.md#the-prompt));
  - `annotate` and `annotate_more`, the eval's silver drafts, which have no single right answer: the oracle answers by a rule a test can predict and that passes every check. Every sentence of five words or more is a fact, quoted whole, core when it holds a digit; the other family adds every sentence of three words or more that its notes do not cite ([eval.md](eval.md#decisions));
  - `outline`, `describe` and `brief`, by rules of the same kind: a section starts at S1 and where a sentence opens with "Now," or "Next,"; a speaker for each "I'm"; a word for each acronym ([pipeline.md](pipeline.md#the-key-and-the-prompts));
  - `atomize`, in all three of its modes: filler words alone are a disfluency and greeting words a greeting; any other sentence is one atom, the sentence itself, running on into the next when it ends without a full stop; a digit makes a number, "?" a question, "maybe" a hedge; capitalized runs are names, acronyms terms, the words between double quotes a must-quote. Asked again, it adds the atom a sentence's atoms miss ([pipeline.md](pipeline.md#the-atomize-stage)).
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
- **End to end, through the fake engine (`--fake-oracle`), arrives with P22.** A call on the wire does not carry its mission kind: the tag is never sent to a model. The fake engine will therefore know a mission by its answer schema's name, which the stages set. Both land together, when there are missions to route.

## Fuzzing

A unit test checks the inputs its author thought of. A fuzz target checks *invariants* on whatever bytes arrive: a captions file in another encoding, a model's broken JSON. It is `LLVMFuzzerTestOneInput()`, which aborts when one breaks (`tests/fuzz/<name>_fuzz.cpp`), and it is built twice (`cmake/RrFuzzing.cmake`):
- **the replay**, `rr_<name>_fuzz_replay`, runs the committed corpus `tests/fuzz/corpus/<name>/` under CTest (`fuzz.<name>`), in every build-test job: GCC, ASan and UBSan, TSan;
- **the search**, `rr_<name>_fuzzer`, is linked with libFuzzer when `RR_FUZZ=ON` (preset `fuzz`: Clang, ASan, UBSan, every target instrumented for coverage). The `fuzz` CI job runs each for two minutes and uploads what broke an invariant.

An input that broke an invariant joins the corpus once fixed, as `regression-*.bin`, and gets a named unit test, so the rule is stated where readers look.

**What is fuzzed:** `rr_text` ([text.md](text.md#fuzzing)), whose fuzzer found its first bug within a second: a word of accents alone folded to an empty term. And the article's `[A#]` marker parser ([knowledge.md](knowledge.md#the-articles-markers)), which ADR-0006 calls correctness- and security-critical. And caption files of every format, read, cut into sentences and projected ([transcript.md](transcript.md#how-the-tests-were-validated)). Within seconds, that target found a NUL byte kept inside a word, and found `rr_json`'s parser throwing on a number too large for a double, which its contract forbids.

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

**What has specs:** `text` (112 mutants), `eval` (101), `agents` (66), `pipeline` (53, the stage store's commit too), `transcript` (53), `reason` (44), `knowledge` (43), `fidelity` (31), `oracle` (29), `prompt` (27), `media` (24), `hub` (10, the job army, the doctor's tools, the media settings), `library` (3, a job's kind), and a few each in `core`, `json` and `llm`, where later phases changed their code. The modules built before P7 were mutation-tested the same way. Their results are recorded in their design notes, and porting the rest of their specs into `tests/mutants/` is follow-up work.

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

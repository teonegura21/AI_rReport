# The army: missions on the pod's engines

A video becomes hundreds of model calls: windows of transcript to atomize, each sampled three times; sections to draft four times; paragraphs to judge. The models are local and fixed, so the quality of an article depends on how those calls are spent. `rr_agents` is where that is decided ([ADR-0011](../adr/0011-commander-infantry.md), [ADR-0018](../adr/0018-compute-tiers-and-ceilings.md)). This note explains what it does and why each rule exists.

## Pieces

| Where | What it is |
|---|---|
| `rr/agents/mission.hpp` | The vocabulary: an `Engine` (what it serves, how much it holds, how it samples), a `Mission` (a role, a conversation, the answer's form and checks), and its `Outcome` |
| `rr/agents/ladder.hpp` | Where a mission goes when its answers fail: the escalation ladder, as a small state machine |
| `rr/agents/sizing.hpp` | Whether a request fits an engine, and what it holds there while it runs |
| `rr/agents/army.hpp` | The `Army`: per-engine battalions, samples, repairs, escalation, ceilings, and the `BattleLog` interface |
| `rr/library/mission_log.hpp` | The battle log, kept in the library database (`missions`, migration 0003) |
| `rr/hub/army_engines.hpp` | The army's engines, from the pod's health report and each family's `[sampling.*]` presets |

A stage says *what* it wants; the army decides *how*:

```cpp
Mission label;
label.id = "atomize/w07";                  // unique in the job
label.kind = "atomize";                    // the calls' purpose
label.prompt = "atomize@3";                // for the battle log
label.role = "infantry";
label.request.messages = {system, task};   // rendered by rr_prompt
label.request.seed = llm::derived_seed(salt, "atomize", label.id, 0);
label.answer_tokens = 1500;                // the answer's size, never max_tokens
label.schema = atoms_schema;               // a JSON answer; none for prose
label.check = every_sentence_accounted_for;
label.samples = 3;                         // the tier's
std::vector<Outcome> outcomes = army.squad(std::move(windows)).value();
```

## Engines and roles

The army is built from the runtime's health report (`hub::army_engines()`). Each engine becomes an `Engine` with:
- its slots, its context per slot, and whether its slots share one pool (`kv_unified`);
- the SHA-256 of the model file it serves (`checksum`): what its model id means now. Stage cache keys hold it, so answers from one file are never taken for another's ([pipeline.md](pipeline.md#the-cache-key));
- the roles it serves;
- its model **family**;
- two sampling presets from the hub's configuration: `[sampling.<family>_direct]` and `[sampling.<family>_thinking]`.

Only the commander's and the infantry's engines take missions. A pod that serves one of the two roles serves both from that engine: the 24 GB profile runs the commander alone, the 16 GB profile light infantry alone. An engine not ready, a role served twice, or a family without its presets is an error that names it.

- **Every role is served by exactly one engine**, and an engine may serve several. On a pod with no infantry, the commander's engine serves both roles, and the ladder then has no commander rung (the mission is already there).
- **The family is the catalog id's leading letters** (`qwen3.8-27b-q4` → `qwen`). It decides where a refusal goes (ADR-0017).
- **A mission's request fills what it leaves unset from the engine's preset** (`llm::apply()`), so a stage that asks the infantry gets Gemma's sampling if Gemma serves it, and Qwen's on a pod where Qwen does. A rung that escalates uses its engine's preset whole: a thinking model at a stage's low temperature degrades.
- **The `thinking` preset must have a budget.** A rung that makes a model think must end: Gemma 4 can loop while thinking.

## Missions, and what the army sets

A mission names a role, never a model; it states `answer_tokens`, never `max_tokens`. Whether a call thinks is the army's choice at each rung, and a call that thinks needs room for its thinking on top of its answer:

> max_tokens = answer_tokens, plus, when the call thinks, the thinking budget and the tokens of its closing words.

Without that, a thinking rung given a stage's answer-sized `max_tokens` would think until the limit and never answer, and every escalation would be wasted. So the army refuses a mission that sets `model`, `max_tokens`, `n`, `json_schema` or the tag itself (`Errc::invalid_argument`), rather than guess which of two values was meant.

**Every mission carries a seed.** Its samples and its escalations sample, and a job's memo (ADR-0020) tells calls apart by their requests. Without a seed, three samples of one window would be three identical requests, and the memo would give all three the first one's answer.

Every mission is checked before any call is made, and a squad with one bad mission does not start. The checks:
- no penalties (they bias labels and paraphrase quotes: [llm.md](llm.md));
- sampling values in range;
- greedy samples refused, since at temperature 0 every sample is the same answer;
- a mission that thinks needs a provider that can think;
- **the mission must fit its engine.** A packet too long for a slot is the stage's to split; the army never cuts one short. The error says how many tokens the mission needs and how many the engine holds.

## Answers: checked, repaired, escalated

Each answer is read in its form:
- **JSON** answers are parsed (a Markdown fence is tolerated) and validated against the full schema. The schema's model-facing form constrains decoding where the provider can, but is never taken as proof.
- **Prose** answers are checked as text; an empty one, or one cut off at the token limit, is a problem. Prose has no closing brace to show it is whole.

Then the stage's own check runs. The steps are `rr_llm`'s own: `structured_request()`, `check_structured()` and `repair_request()`. The army runs the loop itself only so that each attempt takes its own slot.

A failing answer is handled in this order:
1. **A refusal goes to another family, unrepaired.** An answer counts as a refusal when:
   - the server says so (`content_filter`); or
   - the stage's own test says so (`Mission::refusal`, which can also tell a softening); or
   - by default, it is short and opens the way refusals do ("I'm sorry", "I can't", "As an AI"…).

   This test is asked only of answers that already failed their checks, so a valid answer that happens to open that way is never mistaken for one. Repairing a refusal would only ask the same model again.
2. **A call that ran out while thinking is asked again without thinking** (ADR-0018). There is no answer to repair: the thinking used the room.
3. **Otherwise the answer is sent back** with its problems, up to `repairs` times.
4. **The same question is never asked twice.** A retry that would repeat an earlier request of the mission (the commander's no-thinking retry is its first question) gets the same answer, from the memo or the seed, and is skipped.

Repairs are targeted (ADR-0018): the model gets its answer back with what is wrong with it, not "improve this".

### The ladder

When no sample of a rung was accepted, the mission climbs:

| Rung | Engine | Thinking | When |
|---|---|---|---|
| 1. as ordered | the role's | as the mission asked | always |
| 2. thinking | the same | on | unless rung 1 thought |
| 3. commander | the commander's | on | when that is another engine |
| 4. other family | one of a family that has not refused (the commander's first) | on | only after a refusal |

A rung is passed over when:
- the mission does not allow it (`Escalation`). A judge that must stay of another family than the writer it judges turns off rungs 3 and 4;
- it would repeat a rung already climbed (the same engine, thinking the same way);
- its engine's family has refused;
- its engine cannot hold the mission.

The ladder only climbs.

**Why this order.** A small model that fails a narrow, checkable task often succeeds once it thinks, which is far cheaper than the commander. A model that refuses will refuse again however long it thinks, so a refusal skips its family's thinking rung. The evidence for thinking on escalation and for checker-driven repair is in ADR-0018 (CRITIC, Chain-of-Verification, Huang et al. 2024).

**Rungs past the first ask for one answer.** The first rung's samples feed a vote (`rr_reason`). An escalation is a rescue, and its one answer stands for the mission.

**A rung is climbed only when no sample was accepted.** Two of three samples are a vote; the third's problems are kept in `Outcome::problems` for the log.

### Verdicts

| Verdict | Meaning | The stage |
|---|---|---|
| `accepted` | answers passed every check | uses them |
| `invalid` | the ladder's top, without an answer | publishes a finding, with `problems` |
| `refused` | the models declined | publishes a finding |
| `ceiling` | a job ceiling stopped it; it may hold the answers it had | publishes what it has, and a finding |

An **error**, not a verdict, is for what is not the models' doing:
- a mission the army cannot run;
- the job being stopped (`Errc::cancelled`);
- a pod still unreachable after the provider's retries;
- a stage's check that throws (`Errc::internal`).

The first error stops the squad's other missions, and `squad()` returns it. What they had answered is in the job's memo, so a resume asks it again for free.

## Samples share a prompt

A mission's k samples go to its engine as **one request for k answers** where they fit. llama-server reads the prompt once and copies it to k slots, so k samples cost one prompt (plan P3). Where they do not fit at once, they go in several requests. **Sample j draws seed + j** however they are split, because llama-server gives choice i of a request the seed + i. So a split draws the same seeds as one request, and a resumed job asks the same questions.

Each sample is then checked and repaired on its own. A repair is a request for that sample alone, continuing its conversation with its own seed.

## Sizing: slots and tokens

An engine holds a fixed number of tokens ([runtime.md](runtime.md#context-per-slot-and-the-shared-pool)):
- one sequence, a prompt and its answer, at most `ctx_per_slot`;
- in a shared pool, the sequences at work together at most `context`.

When a shared pool overflows, llama-server fails *every* request in flight (verified against the real server: [runtime.md](runtime.md), L4). So each request is sized before it is sent, from:
- an estimate of its prompt (`prompt::TokenEstimator`, which errs high), plus each message's template markers;
- the most each answer may take (`max_tokens`).

The rules, per engine:

| | Each slot has its share | A shared pool |
|---|---|---|
| A request fits when | prompt + max_tokens ≤ ctx_per_slot | the same, and prompt + n × max_tokens ≤ context |
| Answers to one request at once | up to the slots | up to the slots, and as many as the pool holds |
| While it runs, it holds | n slots | n slots, and prompt + n × max_tokens tokens of the pool |

**Reservations are the worst case,** `max_tokens` per answer, though most answers are shorter. Overflowing a shared pool fails every request in flight, so a tighter `answer_tokens` per mission kind, not a gamble on short answers, is how more missions run at once.

**Idle slots need no reservation.** llama-server keeps each idle slot's last prompt cached (prompt reuse), and when a running sequence needs the room it purges them first (`try_clear_idle_slots()`, `tools/server/server-context.cpp` at llama.cpp 7fe450e). Only the requests in flight compete for the pool.

**Slots, then tokens, always in that order.** A request takes its slots, then its share of the pool, and holds both only while its call runs. Nobody holding tokens ever waits for anything, so the two waits cannot deadlock. Both waits are by priority, then first come, first served (`rr::Limiter`).

**A stage asks whether a mission fits, rather than predicting it.** `Army::holds(mission)` applies the squad's own test to the mission as ordered: its engine's presets, its thinking budget and closing words when it thinks, the estimator. A squad refuses a mission that does not fit (`Errc::invalid_argument`, "split its packet") before any runs, so a stage that cuts its packets by an estimate of its own would fail every mission of the squad on the one it got wrong. The eval's `annotate` stage fills each window to a size, then gives up sentences until `holds()` says yes ([eval.md](eval.md#silver-drafted-by-models)). A test pins it at the exact token where a mission stops fitting, with and without thinking.

Images are refused for now (`Errc::unsupported`): their tokens cannot be estimated from a request. The pipeline reads captions (plan M1); images come with the visual stages.

## Battalions: every engine busy, nobody waiting for anybody

Each engine has a **battalion**: a priority queue of mission legs and as many workers as the engine has slots.
- A worker runs one leg: a mission's rung, from its first call to its last repair.
- If the mission must climb to another engine, the worker queues the next leg with *that* engine's battalion and moves on.

So **an infantry worker never waits for the commander.** A mission escalating to a busy commander waits in the commander's queue, while the infantry serves the next window. This is ADR-0011's "second wave", without waiting for a whole wave to end.

The alternative, a pool of mission threads that each block until their mission ends, fails where it matters. On a bad window, dozens of escalations would occupy every thread waiting for one commander, and the infantry would stand idle. The test `EscalationsNeverHoldUpTheInfantry` gives the infantry one slot and holds the commander, and checks that the next infantry mission is served meanwhile.

**Priorities:**
- queues are ordered by priority, then first come, first served;
- a mission keeps its priority up the ladder;
- an escalation joins its new queue at the back of its priority. Throughput is the same either way, and it keeps the queue fair.

**A mission's check must not call the army.** Checks run on a battalion's worker, which would then wait for itself. `squad()` asserts it.

## Ceilings

ADR-0018's job ceilings, completion tokens and wall-clock time, are checked before every call:
- **Once one is reached, no call starts.** Missions end with `Verdict::ceiling` and the answers they had. `Army::ceiling()` says which ceiling, at what spend, and which mission found it, for the stage's finding.
- **Calls in flight finish.** Their tokens are already being spent, and an answer half paid for is better kept. The overshoot is bounded by the calls in flight times their `max_tokens`.
- **Every answer counts**, including those a job's memo gives back. A resumed job re-asks its earlier calls in the same order, so its count reaches the same total. The wall clock counts the time the job spent before (`Options::elapsed_before`).
- A ceiling stays reached: the job is stopping, not pausing.

## The battle log

Each mission, however it ended, leaves one record (`MissionRecord`; the library keeps it in `missions`, migration 0003):
- how it ended (verdict, or error);
- the last rung climbed and how many;
- who answered;
- samples asked and accepted;
- calls, tokens;
- what was wrong with the last answer that failed.

Its calls, one row per attempt, are in `llm_calls` under the same job and mission ids.

A mission run again, by a resumed job, replaces its row. The table refuses rows that contradict themselves (`CHECK` constraints): a verdict and an error, more accepted answers than samples, a rung climbed without a step.

A record that cannot be written is logged, never fatal: the answer is already paid for (the same rule as accounting).

## How it is tested

| Level | What |
|---|---|
| Unit | `ladder_test.cpp`: every rung, refusals by family, passing over, never twice, the commander first, roles sharing an engine, providers that cannot think. `sizing_test.cpp`: markers and rounding, images, split and shared pools at their limits. `mission_test.cpp`: families, refusal openings, names |
| Unit, with a request-keyed fake | `army_test.cpp`. `KeyedProvider` answers by what a request asks, not by when it arrives: a squad's calls arrive in any order, so a scripted fake would be nondeterministic. It can hold a model's calls, to see how many are in flight at once |
| Unit, library | `mission_log_test.cpp`: records kept whole, failures, replacement on resume, order, and the table's own consistency checks |
| Unit, hub | `army_engines_test.cpp`: the 48 GB profile's two engines with their families' presets (which pass the army's checks: an army can be raised on the shipped configuration), one engine serving both roles, and each error |
| TSan | The whole suite, in CI's `tsan` job: battalions, limiters and squads under ThreadSanitizer |

### How the tests were validated

Each rule was removed on purpose (64 mutants), and the named test was confirmed to fail.

| Rule removed | Caught by |
|---|---|
| A request holds a slot per answer | `EachAnswerTakesASlot` |
| A shared pool is reserved at all; for every answer | `ASharedPoolRunsOnlyWhatItsTokensHold`, `ASharedPoolReservesRoomForEveryAnswer` |
| Samples split where they do not fit | `SamplesAreSplitWhereTheyDoNotFitAtOnce` |
| A shared pool's tokens, a slot's share, the prompt kept once | `Sizing.*` |
| Each message's template markers; images refused | `APromptIsItsMessagesAndTheirMarkers` |
| A mission too long for its engine is refused | `RefusesMissionsItCannotRun` (the army's contract assertion) |
| A thinking rung's room for its budget | `ClimbsToThinkingThenToTheCommander` |
| An escalation samples as its engine does | `ClimbsToThinkingThenToTheCommander` |
| No `n` for one answer; sample j's seed, split or repaired | `AnswersAMissionAndSaysWhatItAsked`, `SamplesAreSplitWhereTheyDoNotFitAtOnce`, `EachSampleIsRepairedOnItsOwn` |
| Escalations ask for one answer | `AnEscalationAsksForOneAnswer` |
| Thinking is never sent to a provider that cannot | `NeverAsksAProviderThatCannotThinkToThink` |
| The army's tag on every call | `AnswersAMissionAndSaysWhatItAsked` |
| Seeds, penalties, greedy samples, twin ids refused | `RefusesMissionsItCannotRun` |
| Prose: cut short, empty, its own repair wording | `ProseCutShortOrEmptyIsAProblem`, `ProseIsCheckedAndRepairedToo` |
| A refusal is not repaired; a stage's own test of one; the ladder told | `ARefusalGoesToAnotherFamilyUnrepaired`, `AStageCanSayWhatARefusalLooksLike`, `ACommandersRefusalGoesToTheInfantry` |
| Refused is not invalid | `WhatEveryFamilyRefusesIsAFinding` |
| Out of room while thinking: asked again without thinking | `ThinkingThatRunsOutIsAskedAgainWithoutThinking` |
| The same question is never asked twice | `NeverAsksTheSameQuestionTwice` |
| One accepted sample is enough not to climb | `SomeSamplesAreEnough` |
| The last failure's problems survive a climb | `ClimbsToThinkingThenToTheCommander` |
| A repair too long for the engine is not sent | `ARepairThatWouldNotFitIsNotSent` |
| The ladder: each rung, the commander never its own escalation, no other family without a refusal, refusers passed over, no rung twice, the engine must hold, the commander first, no thinking without a provider that can, the mission's escalation | `Ladder.*` |
| Priority, then first come, first served | `AHigherPriorityGoesFirst` |
| An escalation queued with the engine it needs | `EscalationsNeverHoldUpTheInfantry` |
| Outcomes in the missions' order | `OutcomesComeBackInTheirMissionsOrder` |
| The first error kept; the squad stopped; nothing started after | `TheFirstFailureStopsTheSquad` |
| The caller's stop reaches the missions | `StoppingTheJobStopsItsMissions` |
| A check that throws fails its mission | `ACheckThatThrowsFailsItsMission` |
| Ceilings: before a mission, between samples, before a repair; sticky; counting tokens; the job's earlier time | `ATokenCeilingStopsNewCalls`, `ACeilingMidwayKeepsWhatWasAnswered`, `ACeilingStopsARepair`, `TheWallClockCeilingCountsTheJobsEarlierTime` |
| A record names no rung when none was climbed; who answered; failures recorded | `ATokenCeilingStopsNewCalls`, `TheBattleLogKeepsEveryMission`, `TheFirstFailureStopsTheSquad` |
| Refusals: short only, typographic apostrophes; families lowercased | `Mission.*` |

**What writing the mutants found, before any ran:** six rules had no test that could fail:
- that an answer takes a slot, and a shared pool's room for every answer: the concurrency tests asked for one answer each;
- that escalations ask for one answer;
- that a ceiling stops a repair;
- that nothing starts once a squad has stopped. A free slot is granted even to a stopped mission, so the army checks the stop itself;
- that a repair too long for the engine is not sent.

Each has its test now.

**What running them found:**
- **Tests that held a model's calls could hang instead of failing.** When an early assertion failed, the held calls were never released, and joining the squad's thread waited forever. They now release on every path (`Release`, declared after the thread it unblocks), and wait with deadlines.
- **One test assumed an order that two threads need not keep.** Which of two missions reaches a shared pool first is not fixed, so it now checks the invariant: never both at once.
- **Two mutants survived:**
  - the commander as its own escalation, which is thinking in disguise for a mission that forbade thinking;
  - a battle-log record naming a rung when none was climbed. The library's table would refuse that row.

  Both are tested now.

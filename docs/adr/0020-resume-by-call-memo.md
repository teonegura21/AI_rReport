# ADR-0020: A job resumes from a memo of its model calls

- Status: Accepted
- Date: 2026-10-01

## Context

A job makes thousands of model calls over an hour or more (ADR-0018), and many things can interrupt it:
- the pod stops: idle, preempted, or out of money;
- the hub crashes, or the laptop sleeps;
- the user presses Ctrl-C.

ADR-0004 makes completed *stages* durable: a finished stage is a cache hit. But a stage interrupted at 90% would start again from zero. At the `max` tier the atomize stage alone can take tens of GPU-minutes, so paying for its completed calls twice is not acceptable.

Two facts shape the solution:
- **Sampled calls are not repeatable by default.** llama-server picks a random seed when a request has none, and a GPU's batching makes even a seeded request vary slightly. A resumed job cannot *re-derive* an answer; it can only *remember* it.
- **A request is already a stable key.** `request_key()` hashes everything the model sees (`rr/llm/chat.hpp`). Record and replay (`Recordings`) use it for tests, but they are files outside the library's transactions, and replay refuses new calls: a test tool, not a durability mechanism.

## Decision

1. **`MemoProvider`, a provider decorator,** above the existing stack (`docs/design/llm.md`): memo → retries → accounting, one record per attempt → the runtime's provider. `structured()` calls go through it, repairs included.
   - A call that succeeds is committed to the job's memo in the library database (migration 0002) before its answer is returned.
   - A call whose key is in the memo returns the remembered answer, delivered to the stream sink in one piece. It costs nothing, and it is logged as a memo hit, not as a new call: the job's cost counts each call once.
2. **The memo key is the request key plus the served model.**
   - The request names a *role* ("commander"), and the role's model can change between a crash and a resume: another profile, or another quantization.
   - So the key adds the model file's checksum, as the runtime reports it.
   - A job records the models it started with. `rreport resume` refuses a pod serving different ones, unless told to restart the interrupted stage.
3. **Every request that may sample carries an explicit seed, derived:**
   seed = base + sample index, where base = H(salt, stage, mission key) mod 2<sup>31</sup>.
   - The *mission key* names the mission within its stage, for example its window of sentences.
   - The *salt* is the eval's replicate number, and 0 otherwise. It is not drawn per job: two jobs with the same inputs and configuration ask the same questions, so the stage cache (whose key includes the salt of a stochastic stage, ADR-0004) serves the second one. A user who wants new samples asks for another salt.
   - A memo keyed by the request would otherwise merge *different* samples: three samples of one window are three identical requests without a seed, and all three would get the first one's answer.
   - The derivation is deterministic, so a resumed job asks exactly the questions it asked before and finds their answers.
   - A request that may sample (temperature unset, since servers then sample by default, or above 0) and has no seed is refused as a programming error.
   - **Seeds and `n` choices agree.** llama-server gives choice *i* of a request the seed + *i*. A request for samples *j* to *j* + *n* − 1 is sent with seed base + *j*, so its choices draw the same seeds as *n* single requests would. The samples of one mission never share a seed.
   - Seeds stay below 2<sup>32</sup> − 1, which llama-server reads as "pick one at random".
4. **The memo belongs to its job.**
   - It is deleted when the job's results are committed (artifacts are the truth, ADR-0004), or by `rreport jobs gc` for abandoned jobs.
   - Reuse across jobs remains the stage cache's work. It is keyed by stage version, configuration and inputs (ADR-0004), and its invalidation rules stay the only ones that apply across jobs.
5. **The guarantee:** a resumed job repeats at most the calls that were in flight when it stopped. The end-to-end test (plan P22) proves it: it kills the hub with `kill -9` at random points, resumes, and compares the engines' completion counts with a clean run's.

## Consequences

- **Every stage is resumable without its own checkpoint code,** provided it is deterministic in what it asks: the same inputs, the same prompts, the same derived seeds. That is required anyway, for the stage cache and the eval's common random numbers (ADR-0019).
- **One memo write per call.** At seconds per call, SQLite in WAL mode absorbs this easily (ADR-0008).
- **A remembered answer is reused as given,** even if a GPU would now sample it slightly differently. That is the point: a resumed job continues the run it started.
- **The memo holds answers only while a job runs,** so it never grows without bound, and it can never serve a stale answer to a later job.

## Alternatives considered

- **Stage checkpoints only (ADR-0004 alone).** An interrupted stage loses all its work. Rejected as insufficient.
- **Checkpoints inside each stage.** Every stage would need its own partial-progress artifacts: more code, more bugs, and a resume path per stage. Rejected for one mechanism under all of them.
- **Recordings as the memo.** They are files outside the library's transactions, and replay refuses new calls. Rejected.
- **A memo shared by all jobs** (a cross-job call cache). Sweeps that add samples would reuse the first ones' calls, which is tempting. But it duplicates the stage cache with different invalidation rules, and it risks stale answers after an engine upgrade. Rejected for now; it can return if the eval's sweeps show the cost matters.

# ADR-0005: Closed-world atomization (the Fidelity Ledger)

- Status: Accepted
- Date: 2026-09-29

## Context

The product promise is a *complete* retelling: nothing important dropped. That includes jargon, names, numbers, asides, on-screen content and blunt statements.
- Summarizers fail because the model decides what matters.
- Small models are **unreliable at "find everything important"** but **reliable at "label every row"**.

## Decision

1. **Numbered inputs.** The transcript becomes numbered sentences `S1…Sn`; on-screen items become `V1…Vm`.
2. **One entry per ID.** An atomization mission must return **exactly one entry per ID**: either one or more typed atoms, or a filler label from a closed list (`disfluency`, `greeting`, `repetition`, `sponsor`, `decorative`).
3. **Code checks the structure.** Code verifies that every ID is present (Ledger A) and re-dispatches only the missing IDs.
4. **Atoms carry nuance:** `speaker`, `attributed_to`, `modality` (asserted / hedged / hypothetical / reported / sarcastic / question), `register` (main / aside / joke / anecdote / example), `intensity`, and `must_quote`.
5. **Hard rule.** A sentence labelled filler that contains a number, a named entity or a lexicon term is automatically re-examined.
6. **Ledger B.** Every atom must be cited by at least one article sentence, or explicitly declared a duplicate of another atom.

## Consequences

- Completeness becomes a structural property checked by code, not a model's self-assessment.
- Anti-sanitization becomes largely deterministic: `must_quote` atoms must appear verbatim.
- The Fidelity Report shows exactly what was labelled filler, so users can audit it.
- Cost: more tokens than a summary. We expect verification and repair to add about 15–25% on top of extraction.

## Alternatives considered

- **Open-ended extraction ("list the key points").** This is exactly the failure mode we are fixing. Rejected.
- **One giant long-context pass.** Small-model recall drops with context length, and the result is uncheckable. Rejected.

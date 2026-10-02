// rr/fidelity/checks.hpp — the deterministic checks of an analysis.
//
// Why this exists
//   Models are good at labelling every row and bad at finding everything
//   important (ADR-0005). So a model's work is never trusted for what code
//   can check. At two moments, code checks it all:
//     * after atomize, check_ledger(): the atoms against the transcript. Every
//       sentence accounted for, once; every number and must-quote of an atom
//       actually said in its sentences; no filler hiding a number, a name or
//       a term (Ledger A);
//     * after write, check_article(): the article against the atoms. Every
//       marker valid, every atom told; each atom's numbers, names and terms
//       in the paragraph citing it; must-quotes and every quotation verbatim;
//       the talk's order kept (Ledger B).
//   And when an atom is declared a duplicate of another, so that only the
//   other is told (ADR-0005, rule 6), duplicate_problems() says what telling
//   only the other would lose. The plan stage sends it back to the planner.
//   Each finding's message is what its repair will be told; what cannot be
//   repaired is published in the Fidelity Report (docs/design/fidelity.md).
//
// What the checks hold texts to
//   Texts are compared as rr_text compares them: numbers by value ("37%" is
//   "thirty-seven percent"), names and terms folded ("Curie" is "curie"), and
//   quotes aligned word for word, punctuation, case and accents aside. A
//   bare "one" is not held as a number: it is as often a pronoun as a count.
//   A name is kept when the paragraph gives it whole or by its last word
//   ("Curie" for "Marie Curie").
//   A quotation is what stands between double quotes, curly or straight. A
//   quote mark in code is code, and a straight one after a letter or a digit
//   is an inch mark (13"). A quotation of fewer than three words is a name, a
//   title or a term, and is not held.
//
// Thread-safety: pure functions.
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rr/fidelity/finding.hpp>
#include <rr/knowledge/atoms.hpp>
#include <rr/knowledge/transcript.hpp>

namespace rr::fidelity {

// Ledger A: `ledger` against `transcript`, which may be a window of the whole
// (the sentences one atomize mission was given). A filler may hold none of
// the atoms' terms, nor of `lexicon`: the video's own (its brief's), which a
// window's atoms may not use. Fills the coverage of sentences.
[[nodiscard]] Report check_ledger(const knowledge::Transcript& transcript,
                                  const knowledge::AtomLedger& ledger,
                                  std::span<const std::string> lexicon = {});

// Ledger B: the article `markdown` against `ledger` and `transcript`. Fills
// the coverage of atoms, markers, must-quotes and numbers.
[[nodiscard]] Report check_article(const knowledge::Transcript& transcript,
                                   const knowledge::AtomLedger& ledger, std::string_view markdown);

// Why `atom` is no duplicate of `of`, each said as what telling only `of`
// would lose: a number of its own, by value; a name or a term `of` neither
// says nor lists; words to quote that `of` does not quote; another
// modality, speaker, or source it reports. Empty: it may be declared one.
// Stricter than meaning, on purpose: when in doubt, an atom is told.
[[nodiscard]] std::vector<std::string> duplicate_problems(const knowledge::Atom& atom,
                                                          const knowledge::Atom& of);

}  // namespace rr::fidelity

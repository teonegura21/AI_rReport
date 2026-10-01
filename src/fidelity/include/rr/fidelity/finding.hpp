// rr/fidelity/finding.hpp — what a check found, how to repair it, and what was kept.
//
// Why this exists
//   rReport's promises are checked by code, not hoped for: every sentence
//   accounted for, every atom told, every number right, every quote
//   verbatim (ADR-0005, ADR-0006). A check that fails says what is wrong and
//   what to do, in words a model can act on: the repair mission quotes the
//   message. What a repair cannot fix is published as a finding in the
//   Fidelity Report, never hidden.
//
// Thread-safety: plain values.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rr/knowledge/ids.hpp>
#include <rr/text/span.hpp>

namespace rr::fidelity {

// Every check, by what it holds true (docs/design/fidelity.md).
enum class Check : std::uint8_t {
    // Ledger A: the atoms against the transcript, after atomize.
    sentence_accounted,  // every sentence became atoms, or a filler label
    sentence_once,       // never both
    sentence_known,      // atoms and fillers name sentences of the transcript
    number_said,         // an atom's numbers are in its sentences
    quote_said,          // a must-quote is its sentences' words, verbatim
    name_said,           // an atom's names are in its sentences (a warning)
    filler_empty,        // a filler holds no number, name or lexicon term
    // Ledger B: the article against the atoms, after write.
    marker_valid,     // markers are well formed
    marker_known,     // markers name atoms of the ledger
    atom_told,        // every atom is cited
    number_kept,      // an atom's numbers reach the paragraph citing it
    name_kept,        // so do its names
    term_kept,        // and its lexicon terms
    must_quote_kept,  // a must-quote is in the paragraph citing it, verbatim
    quote_verbatim,   // every quotation of the article is the transcript's words
    order_kept,       // within a section, atoms are told in the order said (a warning)
};

// "sentence-accounted", …: the name reports and repairs use.
[[nodiscard]] std::string_view to_string(Check check) noexcept;

enum class Severity : std::uint8_t {
    error,    // a promise is broken: repaired, or published as a finding
    warning,  // worth a reader's look; it never blocks
};

struct Finding {
    Check check = Check::sentence_accounted;
    Severity severity = Severity::error;
    std::string message{};                           // what is wrong, and what to do about it
    std::vector<knowledge::SentenceId> sentences{};  // the transcript's sentences concerned
    std::vector<knowledge::AtomId> atoms{};          // the atoms concerned
    std::optional<text::Span> article{};             // where in the article

    friend bool operator==(const Finding&, const Finding&) = default;
};

// How much of each promise was kept: the measures of M1's acceptance
// (runbook R3) and of the eval's invariants (ADR-0019).
struct Coverage {
    std::size_t sentences = 0;            // in the transcript
    std::size_t sentences_accounted = 0;  // atoms or a filler label, not both
    std::size_t atoms = 0;                // in the ledger
    std::size_t atoms_told = 0;           // cited by the article
    std::size_t markers = 0;              // every marker written, good or bad
    std::size_t markers_invalid = 0;      // bad, or naming no atom
    std::size_t must_quotes = 0;          // atoms that must be quoted
    std::size_t must_quotes_kept = 0;     // verbatim where they are cited
    std::size_t numbers = 0;              // atoms' numbers that must reach the article
    std::size_t numbers_kept = 0;         // that did

    friend bool operator==(const Coverage&, const Coverage&) = default;
};

struct Report {
    std::vector<Finding> findings{};
    Coverage coverage{};

    [[nodiscard]] std::size_t errors() const noexcept;

    // This report and `other`, as one: findings appended, counts added.
    void add(const Report& other);

    friend bool operator==(const Report&, const Report&) = default;
};

}  // namespace rr::fidelity

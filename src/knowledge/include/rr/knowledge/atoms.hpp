// rr/knowledge/atoms.hpp — Ledger A: what became of every sentence.
//
// Why this exists
//   A summary lets a model decide what matters, and loses what it skipped. So
//   rReport asks models to label every sentence instead (ADR-0005). Each
//   sentence of the transcript becomes one or more atoms, units of what was
//   said, or is labelled filler from a closed list. Code then checks that none
//   is missing (Ledger A), and that every atom reaches the article (Ledger B).
//
//   An atom keeps what a retelling must not lose:
//     * kind: claim, fact, number, example, anecdote, aside, joke, hedge,
//       opinion, question, instruction, definition, reference, or visual;
//     * who says it (speaker), and whom it quotes or reports (attributed_to);
//     * how it is said: modality (asserted, hedged, hypothetical, reported,
//       sarcastic, a question), register (main, an aside, a joke, an
//       anecdote, an example), and intensity (low, medium, high). The article
//       may neither soften nor sharpen it;
//     * must_quote, with the quote: strong, controversial, profane or precise
//       words the article must keep verbatim;
//     * the names and lexicon terms it gives, which must reach the paragraph
//       that cites it.
//   Atom IDs are assigned by code, never by a model (ADR-0006), and its
//   numbers are read from its text by code (rr/text/numbers.hpp).
//
// Thread-safety: plain values.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/json/json.hpp>
#include <rr/knowledge/ids.hpp>

namespace rr::knowledge {

enum class AtomKind : std::uint8_t {
    claim,
    fact,
    number,
    example,
    anecdote,
    aside,
    joke,
    hedge,
    opinion,
    question,
    instruction,
    definition,
    reference,
    visual,
};

enum class Modality : std::uint8_t {
    asserted,
    hedged,
    hypothetical,
    reported,
    sarcastic,
    question,
};

// How a statement sits in the talk ("register" is a C++ keyword).
enum class Register : std::uint8_t {
    main,
    aside,
    joke,
    anecdote,
    example,
};

// How strongly it is said. ADR-0005 names intensity without a scale: three
// levels are what a check of softening and sharpening needs.
enum class Intensity : std::uint8_t {
    low,
    medium,
    high,
};

// Why a sentence holds no atom: the closed list of ADR-0005.
enum class FillerLabel : std::uint8_t {
    disfluency,
    greeting,
    repetition,
    sponsor,
    decorative,
};

[[nodiscard]] std::string_view to_string(AtomKind kind) noexcept;
[[nodiscard]] std::string_view to_string(Modality modality) noexcept;
[[nodiscard]] std::string_view to_string(Register register_kind) noexcept;
[[nodiscard]] std::string_view to_string(Intensity intensity) noexcept;
[[nodiscard]] std::string_view to_string(FillerLabel label) noexcept;

struct Atom {
    AtomId id;
    AtomKind kind = AtomKind::claim;
    std::string text{};                          // what was said, as a statement that stands alone
    std::vector<SentenceId> sentences{};         // where it was said: one or more, in order
    std::optional<std::string> speaker{};        // who says it, when known
    std::optional<std::string> attributed_to{};  // whom it quotes or reports, if anyone
    Modality modality = Modality::asserted;
    Register register_kind = Register::main;
    Intensity intensity = Intensity::medium;
    bool must_quote = false;
    std::optional<std::string> quote{};  // the exact words: present exactly when must_quote
    std::vector<std::string> names{};    // the names it gives: people, places, organizations, works
    std::vector<std::string> terms{};    // the lexicon terms it uses

    friend bool operator==(const Atom&, const Atom&) = default;
};

struct Filler {
    SentenceId sentence;
    FillerLabel label = FillerLabel::disfluency;

    friend bool operator==(const Filler&, const Filler&) = default;
};

struct AtomLedger {
    std::vector<Atom> atoms{};      // A1, A2, … An
    std::vector<Filler> fillers{};  // in the order of their sentences

    // The atom `id`, or nullptr.
    [[nodiscard]] const Atom* find(AtomId id) const noexcept;

    friend bool operator==(const AtomLedger&, const AtomLedger&) = default;
};

[[nodiscard]] json::Json to_json(const AtomLedger& ledger);

// Errc::invalid_argument when `document` breaks the schema
// (schemas/artifacts/atoms.json), or the ledger's own shape: atoms that are
// not A1, A2, … in order; an atom's sentences out of order or repeated; a
// quote without must_quote, or must_quote without a quote; fillers out of the
// order of their sentences, or two for one sentence. Whether the ledger
// accounts for every sentence of a transcript is a fidelity check (plan P13).
[[nodiscard]] Result<AtomLedger> atom_ledger_from_json(const json::Json& document);

}  // namespace rr::knowledge

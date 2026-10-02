// rr/pipeline/drafting.hpp — what the stages that write prose share: an atom
// as a writer is shown it, the room its answer is given, and a draft
// checked, ranked and read back.
//
// Why this exists
//   Two stages write the article's prose: the write stage, a section at a
//   time (rr/pipeline/write_stage.hpp), and the seams stage, the paragraphs
//   where sections meet (rr/pipeline/seams_stage.hpp). Both show a writer
//   its atoms the same way, and hold what it writes to the same rules, so a
//   rule changed for one is changed for both.
//
// What a draft is held to (draft_problems()), each problem said as what to
// do, for the draft to be sent back with it:
//   * no heading: the article has its own;
//   * paragraphs of prose only: no list, no quote set apart, no code;
//   * as many paragraphs as asked, each atom told in its own, and in no
//     other;
//   * every sentence citing the atoms it tells: a sentence that tells none
//     links two others, or brings in what was not said;
//   * every error of Ledger B (rr/fidelity/checks.hpp), held to the atoms
//     asked.
//
// Thread-safety: pure functions.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <rr/knowledge/atoms.hpp>
#include <rr/knowledge/transcript.hpp>

namespace rr::pipeline {

// The room an answer is given, in tokens: at least kLeastAnswer, at most
// kMostAnswer, with kSpareTokens besides what it is sized for.
inline constexpr int kLeastAnswer = 512;
inline constexpr int kMostAnswer = 8'192;
inline constexpr int kSpareTokens = 128;

// What a writer is asked for: each paragraph's atoms, in order.
struct Asked {
    std::vector<std::vector<knowledge::AtomId>> paragraphs{};
    std::vector<knowledge::AtomId> atoms{};  // all of them, in order: what Ledger B holds the draft to
    std::size_t first = 1;                   // the number of the first paragraph asked, in its section
};

// The bytes of `atom` a writer retells: its words, and its words to quote.
[[nodiscard]] std::int64_t atom_bytes(const knowledge::Atom& atom);

// The room an answer telling atoms of `atom_bytes` bytes in `paragraphs`
// paragraphs is given: their words told in at most twice as many, at the
// estimator's three bytes a token; each paragraph's markers and breaks; and
// room besides.
[[nodiscard]] int prose_tokens(std::int64_t atom_bytes, std::size_t paragraphs);

// "A7 [1:02] opinion, aside, high, said by Ada: The speaker was surprised.
// To quote: "…". Names: …. Terms: ….": an atom as a writer is shown it, its
// labels before its words, and what must reach its paragraph after them.
[[nodiscard]] std::string atom_entry(const knowledge::Atom& atom, const knowledge::Transcript& transcript);

// What is wrong with `draft` as the paragraphs `asked` (see the header
// comment), at most kMostProblems of it (rr/pipeline/asking.hpp).
[[nodiscard]] std::vector<std::string> draft_problems(std::string_view draft,
                                                      const knowledge::Transcript& transcript,
                                                      const knowledge::AtomLedger& ledger,
                                                      const Asked& asked);

// The warnings of a draft that passed: Ledger B's, held to the atoms asked.
// What ranks drafts.
[[nodiscard]] std::size_t warnings_of(std::string_view draft, const knowledge::Transcript& transcript,
                                      const knowledge::AtomLedger& ledger, const Asked& asked);

// The paragraphs of a draft that passed, each as written, without the space
// around them.
[[nodiscard]] std::vector<std::string> paragraphs_of(std::string_view draft);

// `draft` as a reader reads it: its paragraphs, without their markers.
[[nodiscard]] std::string plain(std::string_view draft);

// `text` cut to its first 120 bytes at a character's boundary, with "…"
// when cut: a sentence as a message quotes it.
[[nodiscard]] std::string shown(std::string_view text);

}  // namespace rr::pipeline

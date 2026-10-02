// rr/pipeline/readings.hpp — what models made of a window of the transcript,
// sentence by sentence, voted on and reduced to one reading.
//
// Why this exists
//   The atomize stage (rr/pipeline/atomize_stage.hpp) asks the infantry what
//   each sentence of a window says: atoms, or a filler label (ADR-0005). At
//   `max` it asks three times, and keeps what most readings say
//   (self-consistency: Wang et al., 2023; at equal compute it beats asking a
//   model to check itself: Singhi et al., 2025). Everything that turns those
//   readings into one is here, pure and tested on its own:
//
//   What is voted on. Free text never agrees, so the vote is on what a
//   sentence BECAME (verdict_on()): its filler label, or the sorted kinds,
//   modalities and must-quote flags of the atoms that cite it. A hedge read
//   as a fact, a must-quote missed, a sentence dropped as filler: those are
//   what a vote should catch. Register and intensity are left out: readers
//   place them on a scale and differ without either being wrong, so voting
//   on them would contest most sentences and decide nothing.
//
//   One reading, never a patchwork. An atom may cite several sentences (a
//   thought that runs over two), so atoms are never stitched together from
//   different readings: that would split one atom, or tell it twice. The
//   window's atoms come from its BASE reading (base_of()), the one whose
//   verdicts win the most votes.
//
//   What no vote settles. Where the base's verdict is not the winner (it
//   lost the vote, or the vote is tied), its sentences are UNSETTLED
//   (unsettled()). They are grouped into spans, each grown until no atom of
//   the base crosses its edge, so that taking the span out of the base
//   takes whole atoms. At `max` the commander reads each span again,
//   thinking, shown what the readings made of it, and its reading replaces
//   the base's there (replaced()).
//
//   What the atoms barely hold. coverage() says how much of a sentence its
//   atoms hold: the share of its words found in their texts, names, terms
//   and quotes. A sentence held below half is asked again by the recall
//   pass (thorough, max): what does it say that its atoms miss?
//
// Thread-safety: pure functions.
#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/json/json.hpp>
#include <rr/knowledge/atoms.hpp>
#include <rr/knowledge/ids.hpp>
#include <rr/knowledge/transcript.hpp>
#include <rr/reason/vote.hpp>

namespace rr::pipeline {

// What one answer made of a window: atoms numbered A1, A2, … in the order
// written, fillers in the order of their sentences.
struct Reading {
    std::vector<knowledge::Atom> atoms{};
    std::vector<knowledge::Filler> fillers{};

    friend bool operator==(const Reading&, const Reading&) = default;
};

// `answer`, which follows schemas/missions/atomize.json, in the artifact's
// form (schemas/artifacts/atoms.json): atoms numbered A1, A2, … in the
// order written, each citing its row's sentence and those it names in
// "also", in order, once each; its must_quote words as its quote; fillers
// in the order of their rows.
[[nodiscard]] json::Json artifact_of(const json::Json& answer);

// `answer` read (artifact_of(), then knowledge::atom_ledger_from_json()).
// Errc::invalid_argument when it breaks the ledger's own shape: fillers out
// of the order of their sentences, or two for one sentence.
[[nodiscard]] Result<Reading> reading_of(const json::Json& answer);

// What `reading` made of the sentence `id`, as a vote's value: "filler
// greeting"; or "atoms claim/asserted, number/hedged/quote", the atoms
// citing it sorted; or "" when it made nothing of it.
[[nodiscard]] std::string verdict_on(const Reading& reading, knowledge::SentenceId id);

// The ballots `readings` cast on the sentences `ids`: one each, sample by
// sample, the atoms' texts joined as the ballot's text.
[[nodiscard]] std::vector<reason::Ballot> ballots_of(std::span<const Reading> readings,
                                                     std::span<const knowledge::SentenceId> ids);

// The reading whose verdicts on `ids` win the most of `votes` (vote_keyed()
// on the same ids), the first on a tie. 0 for none.
[[nodiscard]] std::size_t base_of(std::span<const Reading> readings,
                                  std::span<const knowledge::SentenceId> ids,
                                  std::span<const reason::Consensus> votes);

// A stretch of sentences, by index into the ids a window was read with.
struct Stretch {
    std::size_t begin = 0;
    std::size_t end = 0;

    friend bool operator==(const Stretch&, const Stretch&) = default;
};

// The stretches of `ids` that `base` does not settle (see the header
// comment): its verdict is not the winner of `votes`. Each grown until no
// atom of `base` crosses its edges; touching ones joined; in order.
[[nodiscard]] std::vector<Stretch> unsettled(const Reading& base, std::span<const knowledge::SentenceId> ids,
                                             std::span<const reason::Consensus> votes);

// `base` with what it made of the sentences of `stretch` replaced by
// `again`'s, atoms renumbered in the order of their first sentences.
[[nodiscard]] Reading replaced(Reading base, std::span<const knowledge::SentenceId> ids, Stretch stretch,
                               const Reading& again);

// How much of `sentence` its `atoms` hold (see the header comment); 1 for a
// sentence without a word.
[[nodiscard]] double coverage(const knowledge::Sentence& sentence, std::span<const knowledge::Atom> atoms);

// The readings of consecutive windows, in order, as one ledger: atoms
// renumbered A1, … in the order of their first sentences, then as read;
// fillers as the windows give them, in the order of their sentences.
[[nodiscard]] knowledge::AtomLedger ledger_of(std::span<const Reading> windows);

}  // namespace rr::pipeline

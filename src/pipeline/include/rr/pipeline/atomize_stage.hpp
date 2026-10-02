// rr/pipeline/atomize_stage.hpp — the stage that writes Ledger A: what became
// of every sentence of the transcript.
//
// Why this exists
//   A summary lets a model decide what matters, and loses what it skipped.
//   So rReport asks models to label every sentence instead (ADR-0005): each
//   becomes one or more atoms, units of what was said, or a filler label
//   from a closed list, and code checks that none is missing
//   (fidelity::check_ledger()). The article is written from the atoms, and
//   must tell every one of them (Ledger B, P20).
//
// How
//   1. The infantry reads the transcript in closed-world windows
//      (rr/pipeline/windows.hpp): about 2,500 bytes of sentences, given up
//      until the infantry holds the mission, with two sentences on either
//      side it may read but not cite. It answers a row a sentence, in order
//      (prompts/atomize): the evidence first (names, terms), then the atom's
//      words, then the judgments. The prompt's system part holds the rules
//      and the video's brief (the outline stage's), the same for every
//      window, for the engine's cache. It thinks if the tier says so; the
//      shipped tiers say no: labelling is narrow and schema-bound, and
//      thinking adds cost, not accuracy (the eval decides, P26).
//   2. Each answer is checked as Ledger A is, on its window, and sent back
//      with what to do: a sentence skipped, written twice, or outside the
//      window; a number its sentence does not say; a must-quote not word for
//      word; a filler that holds a number, a name or one of the video's
//      words; a speaker neither the video's speakers nor the window and its
//      context name.
//   3. Several readings of a window (the tier's samples, 3 at `max`, drawn
//      at once) are voted on sentence by sentence (rr/pipeline/readings.hpp):
//      the reading that wins the most votes gives the window its atoms.
//      Where it does not settle a sentence, the commander reads the stretch
//      again, thinking, shown what each reading made of it, and its reading
//      replaces the base's there (the tier's contested_to_commander).
//      Where the tier does not ask it, the pod serves none, the stretch is
//      too long for it or no answer comes, the base stands, and a finding
//      says the readings disagree.
//   4. The recall pass (the tier's recall_pass): a sentence whose atoms hold
//      less than half its words is asked again, shown alone with its atoms,
//      and what the answer adds is added: never a filler label, never a
//      repeat of an atom it has. Nothing is taken away.
//   5. The windows join into the ledger, A1…An in the order said, checked
//      whole. What still fails is a finding, never hidden: a window no
//      answer read leaves its sentences unaccounted for, and says why.
//
// Without a pod there is no ledger: Errc::needs_user, and the job stops, to
// be resumed once one is set up. More waves of samples where the first
// disagree (the tier's `step`) are not drawn yet: no shipped tier asks for
// them, and a tier that does is refused.
//
// Its key holds the tier's atomize settings; the prompt; the army; what the
// infantry and the commander hold; the windows' bytes and context; the
// least coverage, and what counts as a repeat. It samples: the job's salt is
// in its key.
//
// Thread-safety: immutable.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <rr/pipeline/stage.hpp>

namespace rr::pipeline {

class AtomizeStage final : public Stage {
public:
    static constexpr std::string_view kName = "atomize";

    [[nodiscard]] std::string_view name() const override { return kName; }
    [[nodiscard]] int version() const override { return 1; }
    [[nodiscard]] std::vector<std::string> inputs() const override {
        return {"source", "transcript", "outline"};
    }
    [[nodiscard]] std::string_view schema() const override { return "artifacts/atoms"; }
    [[nodiscard]] bool samples() const override { return true; }

    // Errc::needs_user without a pod; Errc::not_found for a tier rReport
    // does not ship; Errc::unsupported for a tier that asks for waves.
    [[nodiscard]] Result<json::Json> fingerprint(const StageContext& context) const override;

    // Errc::invalid_argument for a transcript without sentences;
    // Errc::internal for an outline that does not cover its transcript.
    // Errors: the army's. What no model could do is a finding.
    [[nodiscard]] Result<StageOutput> run(const StageContext& context) const override;
};

}  // namespace rr::pipeline

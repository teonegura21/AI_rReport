// rr/pipeline/plan_stage.hpp — the stage that plans how the article retells
// the atoms: each section's paragraphs, the atoms each tells, the atoms
// only another tells, and its heading.
//
// Why this exists
//   Ledger A holds what every sentence said (rr/pipeline/atomize_stage.hpp);
//   the article must tell all of it (Ledger B, P20). Deciding where each
//   atom is told before any prose is written keeps that checkable: the
//   writer is handed each paragraph's atoms as a closed list
//   (rr/knowledge/plan.hpp). Planning is where thinking pays (plan §5):
//   grouping what belongs together, finding what is only said again, is
//   judgment over many atoms, and code checks every answer.
//
// How
//   1. Each section of the outline is planned on its own, from its atoms
//      (those whose first sentence it holds), a closed world: the
//      commander, thinking within the tier's budget and at most half of
//      what it holds, sees the section's atoms in the order said
//      (prompts/plan), and the article's sections around it in the shared
//      part. Sections are planned side by side, on the commander's slots.
//   2. Each answer is checked and sent back with what to do: an atom not of
//      the section, told twice, or neither told nor declared a duplicate; a
//      duplicate of itself, of an atom no paragraph tells, or one that would
//      lose what its atom says (fidelity::duplicate_problems()); a paragraph
//      of more than kParagraphAtoms atoms; a heading or a purpose too long.
//   3. A section the commander does not hold is planned in parts, each the
//      most atoms it holds, in order; their paragraphs are joined, and the
//      first part names the heading.
//   4. What no answer planned is grouped in the order said
//      (rr/pipeline/grouping.hpp), with a finding that says why. So is every
//      section at a tier that plans by grouping (`fast`), without a finding:
//      that is the tier's choice. A section whose sentences are all filler
//      has no paragraph.
//
// Without a pod, a tier that plans with the commander has no plan:
// Errc::needs_user, and the job stops, to be resumed once one is set up.
//
// Its key holds the tier's plan settings; the prompt; the army and what the
// commander holds; the limits on paragraphs, headings and purposes. It
// samples: the job's salt is in its key.
//
// Thread-safety: immutable.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <rr/pipeline/stage.hpp>

namespace rr::pipeline {

// The most atoms a planned paragraph tells.
inline constexpr std::size_t kParagraphAtoms = 12;

class PlanStage final : public Stage {
public:
    static constexpr std::string_view kName = "plan";

    [[nodiscard]] std::string_view name() const override { return kName; }
    [[nodiscard]] int version() const override { return 1; }
    [[nodiscard]] std::vector<std::string> inputs() const override {
        return {"source", "transcript", "outline", "atomize"};
    }
    [[nodiscard]] std::string_view schema() const override { return "artifacts/plan"; }
    [[nodiscard]] bool samples() const override { return true; }

    // Errc::needs_user without a pod at a tier that plans with the
    // commander; Errc::not_found for a tier rReport does not ship.
    [[nodiscard]] Result<json::Json> fingerprint(const StageContext& context) const override;

    // Errc::internal for an outline that does not cover its transcript, or
    // a ledger whose atoms are said where no section is. Errors: the
    // army's. What no model could do is a finding.
    [[nodiscard]] Result<StageOutput> run(const StageContext& context) const override;
};

}  // namespace rr::pipeline

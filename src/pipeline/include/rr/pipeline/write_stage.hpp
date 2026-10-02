// rr/pipeline/write_stage.hpp — the stage that writes the article: each
// section's paragraphs, from the plan, every atom told and cited.
//
// Why this exists
//   The plan says which paragraph tells which atoms (rr/pipeline/plan_stage.hpp);
//   this stage writes them as prose a reader wants to read, and holds every
//   sentence to them. A sentence ends with the markers of the atoms it tells
//   ([A12], ADR-0006), so each one traces back to what was said, and code
//   checks the draft as Ledger B checks the article (fidelity::check_article()):
//   every atom told, its numbers, names, terms and words to quote kept, every
//   quotation the transcript's words. Revising works with feedback from
//   outside the model (the plan's evidence: CRITIC, Chain-of-Verification),
//   so a draft that fails is sent back with what failed, never asked to
//   check itself.
//
// How
//   1. Each section of the plan is written on its own, a closed world: the
//      commander, without thinking at the shipped tiers (reasoning models
//      invent more when they restate: the Vectara figures), is given the
//      section's paragraphs, each with its purpose and its atoms in full
//      (prompts/write), and the article's sections around it in the shared
//      part. Sections are written side by side, the tier's drafts each.
//   2. Each draft is checked and sent back with what to do, up to the tier's
//      repair rounds: a heading; more or fewer paragraphs than the plan's;
//      an atom told in another paragraph than its own; a sentence that
//      cites no atom; and every error of Ledger B, held to the section's
//      atoms (a marker of another section's atom is one).
//   3. Of the drafts that pass, the one with fewest warnings is kept (Ledger
//      B's one warning: the talk's order); the first, on a tie. A judge
//      arrives with P20b.
//   4. A section the commander does not hold is written in parts, each the
//      most paragraphs it holds, in order.
//   5. What no draft passed is told as its atoms say it, a paragraph at a
//      time, each atom's words and its marker, with a finding that says why:
//      plain, but faithful.
//   6. The sections join into the article, each under its heading, and the
//      whole is checked as Ledger B, the plan's duplicates excused. What
//      still fails is a finding, never hidden.
//
// Without a pod there is no article: Errc::needs_user, and the job stops, to
// be resumed once one is set up.
//
// Its key holds the tier's write settings; the prompt; the army and what the
// commander holds. It samples: the job's salt is in its key.
//
// Thread-safety: immutable.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <rr/pipeline/stage.hpp>

namespace rr::pipeline {

class WriteStage final : public Stage {
public:
    static constexpr std::string_view kName = "write";

    [[nodiscard]] std::string_view name() const override { return kName; }
    [[nodiscard]] int version() const override { return 1; }
    [[nodiscard]] std::vector<std::string> inputs() const override {
        return {"source", "transcript", "outline", "atomize", "plan"};
    }
    [[nodiscard]] std::string_view schema() const override { return "artifacts/article"; }
    [[nodiscard]] bool samples() const override { return true; }

    // Errc::needs_user without a pod; Errc::not_found for a tier rReport does
    // not ship.
    [[nodiscard]] Result<json::Json> fingerprint(const StageContext& context) const override;

    // Errc::internal for a plan that does not hold its ledger as its outline
    // cuts it. Errors: the army's. What no model could do is a finding.
    [[nodiscard]] Result<StageOutput> run(const StageContext& context) const override;
};

}  // namespace rr::pipeline

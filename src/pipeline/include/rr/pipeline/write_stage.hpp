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
//      an atom told in another paragraph than its own, or again in another;
//      a sentence that cites no atom; and every error of Ledger B, held to
//      the section's atoms (a marker of another section's atom is one)
//      (rr/pipeline/drafting.hpp).
//   3. Where the tier fuses (`fuse`), the commander writes one draft more
//      from those that pass, two or more (Self-MoA: Li, Lin, Xia and Jin,
//      2025, found fusing the best model's own samples beats mixing
//      models'): shown the paragraphs as their writer was, and the drafts
//      best first, as many as it holds (prompts/fuse). The fused draft is
//      held to what a draft is, repaired as one is, never escalated: one
//      that fails is said, and the drafts stand as drawn. One that passes
//      joins them, first among those with as few warnings, since it was
//      written from them all.
//   4. Of the drafts that pass, Ledger B ranks them (its one warning: the
//      talk's order), and where the tier asks for a judge (`judge_top`), the
//      best are read by a model of another family than the writer's: each
//      sentence against its atoms, the evidence quoted before a verdict
//      (prompts/judge), and each pair for how it reads, in both orders
//      (prompts/flow). The draft kept has the fewest issues read, then the
//      fewest warnings, then the fewest comparisons lost
//      (reason::select_lexicographic()); what the judge reads in it is
//      published as its reading. Without a judge, or one that holds the
//      drafts, the first ranked is kept, and it is said.
//   5. A section the commander does not hold is written in parts, each the
//      most paragraphs it holds, in order.
//   6. What no draft passed is told as its atoms say it, a paragraph at a
//      time, each atom's words and its marker, with a finding that says why:
//      plain, but faithful.
//   7. The sections join into the article, each under its heading, and the
//      whole is checked as Ledger B, the plan's duplicates excused. What
//      still fails is a finding, never hidden.
//
// Without a pod there is no article: Errc::needs_user, and the job stops, to
// be resumed once one is set up.
//
// Its key holds the tier's write settings; the prompts (the writer's and the
// judge's, and the fusion's where the tier fuses); the army and what the commander holds, and where the tier
// judges, what the judge holds. It samples: the job's salt is in its key.
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
    [[nodiscard]] int version() const override { return 2; }
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

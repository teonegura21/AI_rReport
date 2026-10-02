// rr/pipeline/seams_stage.hpp — the stage that smooths the article where its
// sections meet: the last paragraph of one and the first of the next.
//
// Why this exists
//   The write stage writes each section apart, a closed world
//   (rr/pipeline/write_stage.hpp): every sentence stays tied to its atoms,
//   but the joins fall as they may. One section ends on the speaker's
//   surprise and the next opens "She says…", pointing back to nobody; a word
//   is said on both sides of a heading; a section starts over. A reader
//   meets the article whole. So the two paragraphs at each seam are written
//   again, together, each keeping its own atoms: only how things are said
//   may change, never what.
//
// How
//   The tier's `[seams] mode` says how far it goes (rr/reason/tier.hpp):
//     * none: the article as written; no model is asked;
//     * rewrite: for each seam, the commander, without thinking, writes the
//       two paragraphs again (prompts/seam), shown the atoms each cites and
//       the paragraphs as written. A rewrite is checked as a draft is
//       (draft_problems(), rr/pipeline/drafting.hpp): two paragraphs, each
//       atom told in its own, every sentence citing, Ledger B held to their
//       atoms; what fails is sent back once. It is kept unless Ledger B
//       warns of more in it than in the paragraphs as written;
//     * recheck: besides, the judge, of another family than the commander
//       (rr/pipeline/judging.hpp), reads the seam as written and as
//       rewritten, and the rewrite is kept only where the judge reads no
//       more issues in it, then Ledger B no more warnings: the order of the
//       write stage's pick, a tie to the rewrite. What the judge reads in a
//       rewrite kept is published as its reading.
//   A seam that stays as written is said, with why. The article is then
//   checked whole by Ledger B, the plan's duplicates excused, and what it
//   finds that the article as written did not have is published.
//
// Where the seams are
//   Found in the article as written, by what its paragraphs cite, never by
//   its headings: a paragraph belongs to the section of the plan whose
//   atoms it cites, and a seam is where the paragraphs of one section give
//   way to the next's. A paragraph's atoms are the ones it cites.
//
// Seams that share a paragraph
//   A section of one paragraph ends one seam and starts the next. Two such
//   seams are not rewritten at once, or the second would undo the first:
//   seams go in two waves. The first holds every seam that shares no
//   paragraph with an earlier seam of the first; the second, the others,
//   each shown its shared paragraph as the first wave left it.
//
// Its key holds the tier's seams mode; with models, the prompts, the army
// and what the commander and, at recheck, the judge hold. It samples: the
// job's salt is in its key.
//
// Thread-safety: immutable.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <rr/pipeline/stage.hpp>

namespace rr::pipeline {

class SeamsStage final : public Stage {
public:
    static constexpr std::string_view kName = "seams";

    [[nodiscard]] std::string_view name() const override { return kName; }
    [[nodiscard]] int version() const override { return 1; }
    [[nodiscard]] std::vector<std::string> inputs() const override {
        return {"source", "transcript", "outline", "atomize", "plan", "write"};
    }
    [[nodiscard]] std::string_view schema() const override { return "artifacts/article"; }
    [[nodiscard]] bool samples() const override { return true; }

    // Errc::needs_user without a pod at a tier that rewrites; Errc::not_found
    // for a tier rReport does not ship.
    [[nodiscard]] Result<json::Json> fingerprint(const StageContext& context) const override;

    // The article, its seams smoothed. Errc::internal for inputs that do not
    // agree: an article that cites an atom no section of its plan tells.
    [[nodiscard]] Result<StageOutput> run(const StageContext& context) const override;
};

}  // namespace rr::pipeline

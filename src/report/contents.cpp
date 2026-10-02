// What a job's page shows, read back from its library (rr/report/contents.hpp).
#include <rr/report/contents.hpp>

#include <format>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <rr/core/error.hpp>
#include <rr/json/json.hpp>
#include <rr/knowledge/article.hpp>
#include <rr/knowledge/atoms.hpp>
#include <rr/knowledge/outline.hpp>
#include <rr/knowledge/plan.hpp>
#include <rr/knowledge/transcript.hpp>
#include <rr/pipeline/atomize_stage.hpp>
#include <rr/pipeline/outline_stage.hpp>
#include <rr/pipeline/plan_stage.hpp>
#include <rr/pipeline/seams_stage.hpp>
#include <rr/pipeline/source.hpp>
#include <rr/pipeline/transcript_stage.hpp>
#include <rr/pipeline/write_stage.hpp>

namespace rr::report {

namespace {

// Puts what `stage` made, `artifact`, in its place in `contents`; the write
// stage's article in `written`, until the seams stage's replaces it.
// Another stage's artifact shows nothing of its own.
Result<void> put(std::string_view stage, const json::Json& artifact, Contents& contents,
                 std::optional<knowledge::Article>& written) {
    if (stage == pipeline::SourceStage::kName) {
        RR_TRY_ASSIGN(const pipeline::Source source, pipeline::source_from_json(artifact));
        contents.video = source.video;
    } else if (stage == pipeline::TranscriptStage::kName) {
        RR_TRY_ASSIGN(contents.transcript, knowledge::transcript_from_json(artifact));
    } else if (stage == pipeline::OutlineStage::kName) {
        RR_TRY_ASSIGN(contents.outline, knowledge::outline_from_json(artifact));
    } else if (stage == pipeline::AtomizeStage::kName) {
        RR_TRY_ASSIGN(contents.ledger, knowledge::atom_ledger_from_json(artifact));
    } else if (stage == pipeline::PlanStage::kName) {
        RR_TRY_ASSIGN(contents.plan, knowledge::plan_from_json(artifact));
    } else if (stage == pipeline::WriteStage::kName) {
        RR_TRY_ASSIGN(written, knowledge::article_from_json(artifact));
    } else if (stage == pipeline::SeamsStage::kName) {
        RR_TRY_ASSIGN(contents.article, knowledge::article_from_json(artifact));
    }
    return ok();
}

}  // namespace

Result<Contents> contents_of(const library::Job& job, const library::StageStore& store) {
    if (job.kind != library::JobKind::article) {
        return fail(
            Errc::invalid_argument,
            std::format("job {} drafts an eval annotation, which has no article: `rreport eval sheet` "
                        "shows what it drafted, with the video's id",
                        job.id));
    }
    RR_TRY_ASSIGN(const std::vector<library::StageRun> runs, store.runs(job.id));
    Contents contents{.job = job.id, .tier = job.tier};
    std::optional<knowledge::Article> written;
    for (const library::StageRun& run : runs) {
        if (!run.artifact.has_value()) {
            continue;  // it failed: it made nothing
        }
        contents.findings.push_back(StageFindings{.stage = run.stage, .findings = run.findings});
        RR_TRY_ASSIGN(const json::Json artifact, store.artifact(*run.artifact));
        RR_TRY(put(run.stage, artifact, contents, written));
    }
    if (!contents.article.has_value()) {
        contents.article = std::move(written);
    }
    return contents;
}

}  // namespace rr::report

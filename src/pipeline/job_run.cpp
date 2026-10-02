// A job taken through its stages (rr/pipeline/job_run.hpp).
#include <rr/pipeline/job_run.hpp>

#include <chrono>
#include <cstdint>
#include <format>
#include <string_view>
#include <utility>
#include <vector>

#include <rr/json/json.hpp>
#include <rr/library/call_memo.hpp>
#include <rr/library/stage_store.hpp>
#include <rr/log/log.hpp>
#include <rr/pipeline/source.hpp>

namespace rr::pipeline {

namespace {

// The title the source stage found, if it ran or was found done.
Result<std::optional<std::string>> title_of(const RunReport& report, const library::StageStore& store) {
    for (const StageReport& stage : report.stages) {
        if (stage.stage == SourceStage::kName) {
            RR_TRY_ASSIGN(const json::Json artifact, store.artifact(stage.artifact));
            RR_TRY_ASSIGN(const Source source, source_from_json(artifact));
            return std::optional<std::string>{source.video.title};
        }
    }
    return std::optional<std::string>{};
}

}  // namespace

Result<JobRun> run_job(library::Job job, const Executor& executor, db::Database& database, const Clock& clock,
                       StageContext context) {
    library::Jobs jobs{database, clock};
    library::StageStore store{database, clock};
    job.status = library::JobStatus::running;
    job.error.reset();
    RR_TRY_ASSIGN(job, jobs.save(job));

    const auto started = clock.steady_now();
    RunReport report = executor.run(job, store, std::move(context));
    job.elapsed += std::chrono::duration_cast<std::chrono::milliseconds>(clock.steady_now() - started);

    RR_TRY_ASSIGN(std::optional<std::string> title, title_of(report, store));
    if (title.has_value()) {
        job.title = std::move(title);
    }
    if (report.error.has_value()) {
        job.status = Executor::status_after(*report.error);
        job.error = report.error->message();
    } else {
        job.status = library::JobStatus::done;
        // Its artifacts are the truth now. A memo that stays only takes room:
        // never a reason to fail a job that is done.
        library::CallMemo memo{database, job.id, clock};
        if (const Result<std::int64_t> forgotten = memo.forget(); !forgotten.has_value()) {
            log::warn("job {} is done, and its memo of model answers could not be forgotten: {}", job.id,
                      forgotten.error().message());
        }
    }
    RR_TRY_ASSIGN(job, jobs.save(job));
    return JobRun{.job = std::move(job), .report = std::move(report)};
}

Result<Millis> video_length(const library::StageStore& store, std::string_view job_id) {
    RR_TRY_ASSIGN(const std::vector<library::StageRun> runs, store.runs(job_id));
    for (const library::StageRun& run : runs) {  // one a stage
        if (run.stage == SourceStage::kName && run.artifact.has_value()) {
            RR_TRY_ASSIGN(const json::Json artifact, store.artifact(*run.artifact));
            RR_TRY_ASSIGN(const Source source, source_from_json(artifact));
            return length_of(source);
        }
    }
    return fail(Errc::not_found,
                std::format("job {} has read no source yet: its video's length is unknown", job_id));
}

}  // namespace rr::pipeline

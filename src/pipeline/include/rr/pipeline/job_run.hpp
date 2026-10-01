// rr/pipeline/job_run.hpp — a job taken through its stages, and where it then
// stands.
//
// Why this exists
//   `rreport ingest` starts a job and `rreport resume` takes one up again.
//   Both run it through the executor (rr/pipeline/executor.hpp), then save
//   where it stands (rr/library/jobs.hpp):
//     * running, before any stage starts: a job the process dies in stays
//       running, where `rreport jobs` shows it and `rreport resume` takes it
//       up;
//     * done, once every stage is done: its memo of model answers is
//       forgotten, since its artifacts are the truth from then on (ADR-0020);
//     * stopped, needs_user or failed, as the error that ended the run says
//       (Executor::status_after()), with that error for the user to read.
//   Whatever the end, the job keeps the title its source gave it, and the
//   time the run took is added to its own: the wall-clock ceiling counts a
//   job's time across resumes (ADR-0018).
//
// Thread-safety: run_job() may be called for different jobs at once.
#pragma once

#include <rr/core/clock.hpp>
#include <rr/core/result.hpp>
#include <rr/db/database.hpp>
#include <rr/library/jobs.hpp>
#include <rr/pipeline/executor.hpp>
#include <rr/pipeline/stage.hpp>

namespace rr::pipeline {

struct JobRun {
    library::Job job{};  // as saved after the run
    RunReport report{};
};

// Runs `job` through `executor` and saves where it stands (see the header
// comment). `database` holds the library. The errors are the library's: what
// stopped the stages is in the report, and in the job.
[[nodiscard]] Result<JobRun> run_job(library::Job job, const Executor& executor, db::Database& database,
                                     const Clock& clock, StageContext context);

}  // namespace rr::pipeline

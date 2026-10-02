// rr/report/contents.hpp — what a job's page shows, read back from its
// library: each stage's artifact, and what each stage could not do.
//
// Why this exists
//   A page is a view of a job's artifacts (rr/report/page.hpp), as far as
//   the job got: one stopped before its article is a page of what it has.
//   The library keeps a job's last run of each stage
//   (rr/library/stage_store.hpp): a stage a resume found done or ran again
//   replaced what it ran before. A run done is shown with its artifact; a
//   run that failed made nothing, so its stage shows nothing, even where an
//   earlier run had done it.
//
//   The article is the seams stage's where it ran, since that is the write
//   stage's article with the paragraphs where sections meet smoothed; until
//   it runs, the write stage's is the article so far.
//
// Thread-safety: as library::StageStore's.
#pragma once

#include <rr/core/result.hpp>
#include <rr/library/jobs.hpp>
#include <rr/library/stage_store.hpp>
#include <rr/report/page.hpp>

namespace rr::report {

// The contents of `job`'s page (see the header comment): each stage done,
// read, with its findings, in the order the stages ran.
// Errors: Errc::invalid_argument for a job that drafts an eval annotation,
// which has no article (rreport eval sheet shows what it drafted); the
// store's; an artifact that does not read as its stage's.
[[nodiscard]] Result<Contents> contents_of(const library::Job& job, const library::StageStore& store);

}  // namespace rr::report

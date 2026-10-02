// rr/library/jobs.hpp — the jobs of a library: what was asked for, and how far
// it got.
//
// Why this exists
//   `rreport ingest` starts a job; `rreport resume` continues one;
//   `rreport jobs` lists them. A job is a row in `jobs`
//   (migrations/0004_pipeline.sql). Its stages' results are kept apart, as
//   artifacts (rr/library/stage_store.hpp): a job holds where it stands.
//
//   A job makes an article (`rreport ingest`), the eval's annotation of a
//   video (`rreport eval annotate`, rr/eval/annotate.hpp), or the eval's
//   judgement of an article (`rreport eval judge`, rr/eval/judgement.hpp):
//   each takes its source through stages of its own, so a job is taken up
//   again only by the command that runs its kind's stages
//   (migrations/0005_job_kind.sql, 0007_judgement.sql).
//
//   A job's status says what happens next:
//     running      its stages are under way (or the hub died while they
//                  were: `resume` continues it)
//     done         every stage is done
//     failed       a stage failed in a way a resume would repeat
//     needs_user   a person must act first: install yt-dlp, pass a bot
//                  check (`rreport doctor` says how)
//     stopped      it was stopped (Ctrl-C), or a ceiling stopped it
//
// Thread-safety: thread-safe (the database serializes writes).
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/clock.hpp>
#include <rr/core/result.hpp>
#include <rr/core/ulid.hpp>
#include <rr/db/database.hpp>

namespace rr::library {

enum class JobStatus : std::uint8_t {
    running,
    done,
    failed,
    needs_user,
    stopped,
};

[[nodiscard]] std::string_view to_string(JobStatus status) noexcept;
// The inverse of to_string(); none for anything else.
[[nodiscard]] std::optional<JobStatus> job_status_from_string(std::string_view name) noexcept;

enum class JobKind : std::uint8_t {
    article,     // a video, through the article's stages (rreport ingest)
    annotation,  // an eval snapshot, through the annotation's (rreport eval annotate)
    judgement,   // an article job's article, read by the eval's judges (rreport eval judge)
    pairing,     // two article jobs' articles, compared by the eval's judges (rreport eval compare --flow)
};

[[nodiscard]] std::string_view to_string(JobKind kind) noexcept;
// The inverse of to_string(); none for anything else.
[[nodiscard]] std::optional<JobKind> job_kind_from_string(std::string_view name) noexcept;

struct Job {
    std::string id{};      // a ULID
    std::string source{};  // a web address, a caption file's path, or the job judged
    std::string tier{};    // fast | thorough | max; a judgement's panel
    std::int64_t salt = 0;
    JobStatus status = JobStatus::running;
    std::optional<std::string> error{};   // why it is not done, as the user is told
    std::optional<std::string> title{};   // the video's, once known
    std::chrono::milliseconds elapsed{};  // time spent running, across resumes
    std::chrono::system_clock::time_point created_at{};
    std::chrono::system_clock::time_point updated_at{};
    JobKind kind = JobKind::article;

    friend bool operator==(const Job&, const Job&) = default;
};

class Jobs {
public:
    // `database` must have the library schema (open_database()); it and
    // `clock` must outlive this.
    Jobs(db::Database& database, const Clock& clock);

    // A new job of `kind`, running, with a new ID: later than every ID this
    // Jobs gave.
    [[nodiscard]] Result<Job> create(std::string source, std::string tier, std::int64_t salt,
                                     JobKind kind = JobKind::article);

    // The job `id`; Errc::not_found when there is none.
    [[nodiscard]] Result<Job> find(std::string_view id) const;

    // The most recent jobs, newest first, at most `limit`.
    [[nodiscard]] Result<std::vector<Job>> recent(std::size_t limit) const;

    // Saves `job`'s status, error, title and elapsed time, and stamps it
    // updated. Errc::not_found for a job never created.
    [[nodiscard]] Result<Job> save(Job job);

private:
    db::Database* database_;
    const Clock* clock_;
    std::unique_ptr<UlidGenerator> ids_;  // a pointer: the generator holds a mutex, and Jobs moves
};

}  // namespace rr::library

#include <rr/library/jobs.hpp>

#include <array>
#include <chrono>
#include <format>
#include <memory>
#include <utility>

namespace rr::library {

namespace {

constexpr auto kStatuses = std::to_array<std::pair<JobStatus, std::string_view>>({
    {JobStatus::running, "running"},
    {JobStatus::done, "done"},
    {JobStatus::failed, "failed"},
    {JobStatus::needs_user, "needs_user"},
    {JobStatus::stopped, "stopped"},
});

constexpr auto kKinds = std::to_array<std::pair<JobKind, std::string_view>>({
    {JobKind::article, "article"},
    {JobKind::annotation, "annotation"},
});

std::int64_t millis(std::chrono::system_clock::time_point at) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count();
}

std::chrono::system_clock::time_point at(std::int64_t millis) {
    return std::chrono::system_clock::time_point{std::chrono::milliseconds{millis}};
}

constexpr std::string_view kColumns =
    "id, source, tier, salt, status, error, title, elapsed_ms, created_at_ms, updated_at_ms, kind";

Result<Job> job_of(const db::Row& row) {
    const std::string status = row.get_text(4);
    const std::optional<JobStatus> parsed = job_status_from_string(status);
    if (!parsed.has_value()) {
        return fail(Errc::integrity, std::format("job {} has the status '{}', which rReport does not know",
                                                 row.get_text(0), status));
    }
    const std::string kind = row.get_text(10);
    const std::optional<JobKind> made = job_kind_from_string(kind);
    if (!made.has_value()) {
        return fail(Errc::integrity,
                    std::format("job {} makes '{}', which rReport does not know", row.get_text(0), kind));
    }
    return Job{.id = row.get_text(0),
               .source = row.get_text(1),
               .tier = row.get_text(2),
               .salt = row.get_int(3),
               .status = *parsed,
               .error = row.get_optional_text(5),
               .title = row.get_optional_text(6),
               .elapsed = std::chrono::milliseconds{row.get_int(7)},
               .created_at = at(row.get_int(8)),
               .updated_at = at(row.get_int(9)),
               .kind = *made};
}

}  // namespace

std::string_view to_string(JobStatus status) noexcept {
    for (const auto& [known, name] : kStatuses) {
        if (known == status) {
            return name;
        }
    }
    return "unknown";
}

std::optional<JobStatus> job_status_from_string(std::string_view name) noexcept {
    for (const auto& [status, known] : kStatuses) {
        if (known == name) {
            return status;
        }
    }
    return std::nullopt;
}

std::string_view to_string(JobKind kind) noexcept {
    for (const auto& [known, name] : kKinds) {
        if (known == kind) {
            return name;
        }
    }
    return "unknown";
}

std::optional<JobKind> job_kind_from_string(std::string_view name) noexcept {
    for (const auto& [kind, known] : kKinds) {
        if (known == name) {
            return kind;
        }
    }
    return std::nullopt;
}

Jobs::Jobs(db::Database& database, const Clock& clock)
    : database_(&database)
    , clock_(&clock)
    , ids_(std::make_unique<UlidGenerator>(clock)) {}

Result<Job> Jobs::create(std::string source, std::string tier, std::int64_t salt, JobKind kind) {
    const auto now = clock_->now();
    Job job{.id = ids_->next(),
            .source = std::move(source),
            .tier = std::move(tier),
            .salt = salt,
            .created_at = std::chrono::time_point_cast<std::chrono::milliseconds>(now),
            .updated_at = std::chrono::time_point_cast<std::chrono::milliseconds>(now),
            .kind = kind};
    RR_TRY(database_->write([&](db::WriteTx& tx) -> Result<void> {
        RR_TRY(tx.execute(
            std::format("INSERT INTO jobs ({}) VALUES (?1, ?2, ?3, ?4, ?5, NULL, NULL, 0, ?6, ?6, ?7)",
                        kColumns),
            {job.id, job.source, job.tier, job.salt, to_string(job.status), millis(now),
             to_string(job.kind)}));
        return ok();
    }));
    return job;
}

Result<Job> Jobs::find(std::string_view id) const {
    return database_->read([&](db::ReadTx& tx) -> Result<Job> {
        std::optional<Job> found;
        RR_TRY(tx.query(std::format("SELECT {} FROM jobs WHERE id = ?1", kColumns), {id},
                        [&](const db::Row& row) -> Result<void> {
                            RR_TRY_ASSIGN(found, job_of(row));
                            return ok();
                        }));
        if (!found.has_value()) {
            return fail(Errc::not_found, std::format("no job {}: `rreport jobs` lists them", id));
        }
        return *std::move(found);
    });
}

Result<std::vector<Job>> Jobs::recent(std::size_t limit) const {
    return database_->read([&](db::ReadTx& tx) -> Result<std::vector<Job>> {
        std::vector<Job> jobs;
        RR_TRY(tx.query(std::format("SELECT {} FROM jobs ORDER BY id DESC LIMIT ?1", kColumns),
                        {static_cast<std::int64_t>(limit)}, [&](const db::Row& row) -> Result<void> {
                            RR_TRY_ASSIGN(Job job, job_of(row));
                            jobs.push_back(std::move(job));
                            return ok();
                        }));
        return jobs;
    });
}

Result<Job> Jobs::save(Job job) {
    const auto now = clock_->now();
    job.updated_at = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
    RR_TRY_ASSIGN(const std::int64_t changed, database_->write([&](db::WriteTx& tx) {
        return tx.execute(
            "UPDATE jobs SET status = ?2, error = ?3, title = ?4, elapsed_ms = ?5, updated_at_ms = ?6 WHERE "
            "id = ?1",
            {job.id, to_string(job.status), job.error, job.title, job.elapsed.count(), millis(now)});
    }));
    if (changed == 0) {
        return fail(Errc::not_found, std::format("no job {} to save", job.id));
    }
    return job;
}

}  // namespace rr::library

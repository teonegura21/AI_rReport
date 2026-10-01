#include <rr/library/stage_store.hpp>

#include <chrono>
#include <cstddef>
#include <format>
#include <span>
#include <string>
#include <utility>

#include <rr/core/hash.hpp>

namespace rr::library {

namespace {

std::int64_t millis(std::chrono::system_clock::time_point at) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count();
}

std::chrono::system_clock::time_point at(std::int64_t millis) {
    return std::chrono::system_clock::time_point{std::chrono::milliseconds{millis}};
}

std::span<const std::byte> bytes_of(std::string_view text) {
    return std::as_bytes(std::span{text.data(), text.size()});
}

std::string text_of(const db::Blob& bytes) {
    std::string text(bytes.size(), '\0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        text[index] = static_cast<char>(bytes[index]);
    }
    return text;
}

// A job's run of a stage, written over an earlier run of it: a resumed job
// runs a failed stage again.
Result<void> record_run(db::WriteTx& tx, const StageRun& run) {
    RR_TRY(tx.execute(
        "INSERT INTO stage_runs (job_id, stage, key, status, cached, artifact, findings, error, "
        "started_at_ms, finished_at_ms) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10) "
        "ON CONFLICT (job_id, stage) DO UPDATE SET key = excluded.key, status = excluded.status, "
        "cached = excluded.cached, artifact = excluded.artifact, findings = excluded.findings, "
        "error = excluded.error, started_at_ms = excluded.started_at_ms, "
        "finished_at_ms = excluded.finished_at_ms",
        {run.job_id, run.stage, run.key, to_string(run.status), run.cached, run.artifact,
         json::dump(json::Json(run.findings)), run.error, millis(run.started_at), millis(run.finished_at)}));
    return ok();
}

Result<std::vector<std::string>> findings_of(std::string_view written) {
    RR_TRY_ASSIGN(const json::Json document, json::parse(written));
    std::vector<std::string> findings;
    if (!document.is_array()) {
        return fail(Errc::integrity, "a stage's findings are not a list");
    }
    for (const json::Json& finding : document) {
        if (!finding.is_string()) {
            return fail(Errc::integrity, "a stage's finding is not a message");
        }
        findings.push_back(finding.get<std::string>());
    }
    return findings;
}

}  // namespace

std::string_view to_string(StageStatus status) noexcept {
    return status == StageStatus::done ? "done" : "failed";
}

StageStore::StageStore(db::Database& database, const Clock& clock) : database_(&database), clock_(&clock) {}

Result<std::optional<CachedResult>> StageStore::cached(std::string_view key) const {
    return database_->read([&](db::ReadTx& tx) -> Result<std::optional<CachedResult>> {
        std::optional<CachedResult> found;
        RR_TRY(tx.query("SELECT artifact, findings FROM stage_cache WHERE key = ?1", {key},
                        [&](const db::Row& row) -> Result<void> {
                            RR_TRY_ASSIGN(std::vector<std::string> findings, findings_of(row.get_text(1)));
                            found =
                                CachedResult{.artifact = row.get_text(0), .findings = std::move(findings)};
                            return ok();
                        }));
        return found;
    });
}

Result<json::Json> StageStore::artifact(std::string_view hash) const {
    return database_->read([&](db::ReadTx& tx) -> Result<json::Json> {
        std::optional<std::string> body;
        RR_TRY(tx.query("SELECT codec, body FROM artifacts WHERE hash = ?1", {hash},
                        [&](const db::Row& row) -> Result<void> {
                            if (row.get_text(0) != "json") {
                                return fail(Errc::unsupported,
                                            std::format("artifact {} is kept as '{}', which "
                                                        "this rReport cannot read",
                                                        hash, row.get_text(0)));
                            }
                            body = text_of(row.get_blob(1));
                            return ok();
                        }));
        if (!body.has_value()) {
            return fail(Errc::not_found, std::format("no artifact {}", hash));
        }
        if (blake3(*body).hex() != hash) {
            return fail(Errc::integrity,
                        std::format("artifact {} is not what its hash says: the library is damaged", hash));
        }
        return json::parse(*body);
    });
}

Result<std::string> StageStore::commit(const StageResult& result) {
    const std::string body = json::dump(result.artifact);
    std::string hash = blake3(body).hex();
    const auto now = clock_->now();
    RR_TRY(database_->write([&](db::WriteTx& tx) -> Result<void> {
        RR_TRY(tx.execute(
            "INSERT INTO artifacts (hash, schema, codec, body, bytes, created_at_ms) VALUES (?1, ?2, "
            "'json', ?3, ?4, ?5) ON CONFLICT (hash) DO NOTHING",
            {hash, result.schema, bytes_of(body), static_cast<std::int64_t>(body.size()), millis(now)}));
        if (result.reusable) {
            // Two jobs that did the same stage at once found the same answer:
            // the first entry stays.
            RR_TRY(tx.execute(
                "INSERT INTO stage_cache (key, stage, version, artifact, findings, created_at_ms) VALUES "
                "(?1, ?2, ?3, ?4, ?5, ?6) ON CONFLICT (key) DO NOTHING",
                {result.key, result.stage, result.version, hash, json::dump(json::Json(result.findings)),
                 millis(now)}));
        }
        return record_run(tx, StageRun{.job_id = result.job_id,
                                       .stage = result.stage,
                                       .key = result.key,
                                       .status = StageStatus::done,
                                       .artifact = hash,
                                       .findings = result.findings,
                                       .started_at = result.started_at,
                                       .finished_at = now});
    }));
    return hash;
}

Result<void> StageStore::reuse(std::string_view job_id, std::string_view stage, std::string_view key,
                               const CachedResult& cached, std::chrono::system_clock::time_point started_at) {
    return database_->write([&](db::WriteTx& tx) {
        return record_run(tx, StageRun{.job_id = std::string{job_id},
                                       .stage = std::string{stage},
                                       .key = std::string{key},
                                       .status = StageStatus::done,
                                       .cached = true,
                                       .artifact = cached.artifact,
                                       .findings = cached.findings,
                                       .started_at = started_at,
                                       .finished_at = clock_->now()});
    });
}

Result<void> StageStore::record_failure(std::string_view job_id, std::string_view stage, std::string_view key,
                                        std::string_view error,
                                        std::chrono::system_clock::time_point started_at) {
    return database_->write([&](db::WriteTx& tx) {
        return record_run(tx, StageRun{.job_id = std::string{job_id},
                                       .stage = std::string{stage},
                                       .key = std::string{key},
                                       .status = StageStatus::failed,
                                       .error = std::string{error},
                                       .started_at = started_at,
                                       .finished_at = clock_->now()});
    });
}

Result<std::vector<StageRun>> StageStore::runs(std::string_view job_id) const {
    return database_->read([&](db::ReadTx& tx) -> Result<std::vector<StageRun>> {
        std::vector<StageRun> runs;
        RR_TRY(
            tx.query("SELECT job_id, stage, key, status, cached, artifact, findings, error, started_at_ms, "
                     "finished_at_ms FROM stage_runs WHERE job_id = ?1 ORDER BY started_at_ms, stage",
                     {job_id}, [&](const db::Row& row) -> Result<void> {
                         RR_TRY_ASSIGN(std::vector<std::string> findings, findings_of(row.get_text(6)));
                         runs.push_back(StageRun{
                             .job_id = row.get_text(0),
                             .stage = row.get_text(1),
                             .key = row.get_text(2),
                             .status = row.get_text(3) == "done" ? StageStatus::done : StageStatus::failed,
                             .cached = row.get_int(4) != 0,
                             .artifact = row.get_optional_text(5),
                             .findings = std::move(findings),
                             .error = row.get_optional_text(7),
                             .started_at = at(row.get_int(8)),
                             .finished_at = at(row.get_int(9))});
                         return ok();
                     }));
        return runs;
    });
}

}  // namespace rr::library

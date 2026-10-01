#include <rr/library/call_memo.hpp>

#include <chrono>
#include <format>
#include <utility>

#include <rr/json/json.hpp>

namespace rr::library {

CallMemo::CallMemo(db::Database& database, std::string job_id, const Clock& clock)
    : database_(&database)
    , job_id_(std::move(job_id))
    , clock_(&clock) {}

Result<std::optional<llm::ChatResponse>> CallMemo::find(std::string_view key) const {
    return database_->read([&](db::ReadTx& tx) -> Result<std::optional<llm::ChatResponse>> {
        std::optional<llm::ChatResponse> found;
        RR_TRY(tx.query("SELECT response FROM llm_memo WHERE job_id = ?1 AND memo_key = ?2", {job_id_, key},
                        [&](const db::Row& row) -> Result<void> {
                            RR_TRY_ASSIGN(const json::Json document, json::parse(row.get_text(0)));
                            RR_TRY_ASSIGN(llm::ChatResponse response, llm::response_from_json(document));
                            found = std::move(response);
                            return ok();
                        }));
        return found;
    });
}

Result<void> CallMemo::keep(std::string_view key, const llm::ChatResponse& response) {
    const std::int64_t now =
        std::chrono::duration_cast<std::chrono::milliseconds>(clock_->now().time_since_epoch()).count();
    return database_->write([&](db::WriteTx& tx) -> Result<void> {
        // The first answer kept under a key stays: a second one for the same
        // question is not a better answer, only a later one.
        RR_TRY(
            tx.execute("INSERT INTO llm_memo (job_id, memo_key, response, created_at_ms) VALUES (?1, ?2, ?3, "
                       "?4) ON CONFLICT (job_id, memo_key) DO NOTHING",
                       {job_id_, key, json::dump(llm::to_json(response)), now}));
        return ok();
    });
}

Result<std::int64_t> CallMemo::forget() {
    return database_->write(
        [&](db::WriteTx& tx) { return tx.execute("DELETE FROM llm_memo WHERE job_id = ?1", {job_id_}); });
}

}  // namespace rr::library

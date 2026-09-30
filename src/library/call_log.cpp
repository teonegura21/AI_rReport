#include <rr/library/call_log.hpp>

#include <algorithm>
#include <format>
#include <limits>
#include <utility>

namespace rr::library {

namespace {

std::int64_t unix_ms(std::chrono::system_clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}

// Empty strings are stored as NULL: "no job", not "a job with no name".
std::optional<std::string> or_null(const std::string& text) {
    return text.empty() ? std::nullopt : std::optional<std::string>{text};
}

llm::CallRecord call_from(const db::Row& row) {
    llm::CallRecord call;
    call.id = row.get_text(0);
    call.started_at = std::chrono::system_clock::time_point{std::chrono::milliseconds{row.get_int(1)}};
    call.duration = std::chrono::milliseconds{row.get_int(2)};
    if (const auto first_token = row.get_optional_int(3)) {
        call.first_token = std::chrono::milliseconds{*first_token};
    }
    call.provider = row.get_text(4);
    call.model = row.get_text(5);
    call.tag = llm::CallTag{.purpose = row.get_text(6),
                            .job_id = row.get_optional_text(7).value_or(""),
                            .mission_id = row.get_optional_text(8).value_or("")};
    call.request_key = row.get_text(9);
    if (const std::string status = row.get_text(10); status != "ok") {
        call.error =
            Error{errc_from_string(status).value_or(Errc::internal), row.get_optional_text(11).value_or("")};
    }
    if (const auto finish = row.get_optional_text(12)) {
        call.finish = llm::finish_reason_from_string(*finish);
    }
    call.usage = llm::Usage{.prompt_tokens = row.get_int(13),
                            .completion_tokens = row.get_int(15),
                            .cached_prompt_tokens = row.get_int(14)};
    return call;
}

}  // namespace

Result<void> CallLog::record(const llm::CallRecord& call) {
    const std::optional<std::int64_t> first_token =
        call.first_token.has_value() ? std::optional{call.first_token->count()} : std::nullopt;
    const std::string status = call.error.has_value() ? std::string{to_string(call.error->code())} : "ok";
    const std::optional<std::string> error =
        call.error.has_value() ? std::optional{call.error->describe()} : std::nullopt;
    const std::optional<std::string> finish =
        call.finish.has_value() ? std::optional{std::string{llm::to_string(*call.finish)}} : std::nullopt;

    return database_->write([&](db::WriteTx& tx) -> Result<void> {
        RR_TRY(
            tx.execute("INSERT INTO llm_calls (id, started_at_ms, duration_ms, first_token_ms, provider, "
                       "model, purpose, "
                       "job_id, mission_id, request_key, status, error, finish_reason, prompt_tokens, "
                       "cached_prompt_tokens, "
                       "completion_tokens) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, "
                       "?14, ?15, ?16)",
                       {call.id, unix_ms(call.started_at), call.duration.count(), first_token, call.provider,
                        call.model, call.tag.purpose, or_null(call.tag.job_id), or_null(call.tag.mission_id),
                        call.request_key, status, error, finish, call.usage.prompt_tokens,
                        call.usage.cached_prompt_tokens, call.usage.completion_tokens}));
        RR_TRY(tx.execute("INSERT INTO llm_call_payloads (call_id, request, response) VALUES (?1, ?2, ?3)",
                          {call.id, json::dump(call.request), json::dump(call.response)}));
        return ok();
    });
}

Result<std::vector<llm::CallRecord>> CallLog::list(const CallQuery& query) const {
    const auto limit = static_cast<std::int64_t>(std::min<std::size_t>(
        query.limit, static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())));
    return database_->read([&](db::ReadTx& tx) -> Result<std::vector<llm::CallRecord>> {
        std::vector<llm::CallRecord> calls;
        RR_TRY(tx.query(
            "SELECT id, started_at_ms, duration_ms, first_token_ms, provider, model, purpose, job_id, "
            "mission_id, request_key, status, error, finish_reason, prompt_tokens, cached_prompt_tokens, "
            "completion_tokens FROM llm_calls "
            "WHERE (?1 IS NULL OR job_id = ?1) AND (?2 IS NULL OR purpose = ?2) "
            "ORDER BY started_at_ms DESC, id DESC LIMIT ?3",
            {query.job_id, query.purpose, limit}, [&](const db::Row& row) -> Result<void> {
                calls.push_back(call_from(row));
                return ok();
            }));
        return calls;
    });
}

Result<CallPayload> CallLog::payload(std::string_view call_id) const {
    return database_->read([&](db::ReadTx& tx) -> Result<CallPayload> {
        std::optional<CallPayload> found;
        RR_TRY(tx.query("SELECT request, response FROM llm_call_payloads WHERE call_id = ?1", {call_id},
                        [&](const db::Row& row) -> Result<void> {
                            RR_TRY_ASSIGN(json::Json request, json::parse(row.get_text(0)));
                            RR_TRY_ASSIGN(json::Json response, json::parse(row.get_text(1)));
                            found =
                                CallPayload{.request = std::move(request), .response = std::move(response)};
                            return ok();
                        }));
        if (!found.has_value()) {
            return fail(Errc::not_found, std::format("no payload is kept for call '{}'", call_id));
        }
        return std::move(*found);
    });
}

Result<std::int64_t> CallLog::prune_payloads(std::chrono::system_clock::time_point before) {
    return database_->write([&](db::WriteTx& tx) {
        return tx.execute(
            "DELETE FROM llm_call_payloads WHERE call_id IN (SELECT id FROM llm_calls WHERE started_at_ms < "
            "?1)",
            {unix_ms(before)});
    });
}

}  // namespace rr::library

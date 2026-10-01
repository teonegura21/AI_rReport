// rr/library/call_memo.hpp — a job's memory of its model calls, kept in the
// library database.
//
// Why this exists
//   rr_llm's MemoProvider answers a resumed job's calls from memory
//   (rr/llm/memo.hpp, ADR-0020). CallMemo is the store that remembers them:
//   one row per answer in llm_memo (migrations/0002_call_memo.sql), keyed by
//   the job and the call's memo key, committed as each answer arrives. It
//   lives in the library's database, next to the job's artifacts, so it is
//   as durable as they are. Once a job's results are committed, its answers
//   are forgotten (forget()): from then on the artifacts are the truth.
//
// Thread-safety: thread-safe (the database serializes writes).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <rr/core/clock.hpp>
#include <rr/core/result.hpp>
#include <rr/db/database.hpp>
#include <rr/llm/memo.hpp>

namespace rr::library {

class CallMemo final : public llm::MemoStore {
public:
    // One job's memo. `database` must have the library schema
    // (open_database()); it and `clock` must outlive the memo.
    CallMemo(db::Database& database, std::string job_id, const Clock& clock);

    [[nodiscard]] Result<std::optional<llm::ChatResponse>> find(std::string_view key) const override;
    [[nodiscard]] Result<void> keep(std::string_view key, const llm::ChatResponse& response) override;

    // Forgets every answer of this job; returns how many there were.
    [[nodiscard]] Result<std::int64_t> forget();

private:
    db::Database* database_;
    std::string job_id_;
    const Clock* clock_;
};

}  // namespace rr::library

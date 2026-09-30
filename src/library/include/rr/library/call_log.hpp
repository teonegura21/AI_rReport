// rr/library/call_log.hpp — every model call, kept in the library database.
//
// Why this exists
//   rr_llm's AccountedProvider hands one CallRecord per attempt at a model
//   call to a CallRecorder (rr/llm/accounting.hpp). CallLog is the recorder
//   that keeps them: the numbers in llm_calls, and what was sent and
//   received in llm_call_payloads (migrations/0001_accounting.sql). Keeping
//   the two apart lets a retention policy drop payloads, which are large,
//   while the accounting stays complete.
//
// Thread-safety: thread-safe (the database serializes writes).
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/db/database.hpp>
#include <rr/json/json.hpp>
#include <rr/llm/accounting.hpp>

namespace rr::library {

struct CallQuery {
    std::optional<std::string> job_id{};
    std::optional<std::string> purpose{};
    std::size_t limit = 100;
};

struct CallPayload {
    json::Json request{};
    json::Json response{};
};

class CallLog final : public llm::CallRecorder {
public:
    // `database` must have the library schema (open_database()) and outlive the log.
    explicit CallLog(db::Database& database) : database_(&database) {}

    [[nodiscard]] Result<void> record(const llm::CallRecord& call) override;

    // Matching calls, newest first, without their payloads.
    [[nodiscard]] Result<std::vector<llm::CallRecord>> list(const CallQuery& query) const;

    // What one call sent and received: Errc::not_found for an unknown call,
    // or one whose payload was pruned.
    [[nodiscard]] Result<CallPayload> payload(std::string_view call_id) const;

    // Deletes the payloads of calls that started before `before` and keeps
    // the calls. Returns how many payloads were deleted.
    [[nodiscard]] Result<std::int64_t> prune_payloads(std::chrono::system_clock::time_point before);

private:
    db::Database* database_;
};

}  // namespace rr::library

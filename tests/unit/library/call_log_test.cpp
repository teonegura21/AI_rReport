#include <rr/library/call_log.hpp>

#include <chrono>
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/library/library.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

namespace rr::library {
namespace {

using namespace std::chrono_literals;
using ::testing::HasSubstr;
using testkit::unwrap;

const std::chrono::system_clock::time_point kNoon{std::chrono::milliseconds{1'790'000'000'000}};

llm::CallRecord a_call(std::string id, std::chrono::system_clock::time_point started_at) {
    return llm::CallRecord{
        .id = std::move(id),
        .started_at = started_at,
        .duration = 2500ms,
        .first_token = 400ms,
        .provider = "runtime",
        .model = "commander",
        .tag = {.purpose = "compose", .job_id = "J1", .mission_id = "M7"},
        .request_key = std::string(64, 'a'),
        .finish = llm::FinishReason::stop,
        .usage = {.prompt_tokens = 1200, .completion_tokens = 300, .cached_prompt_tokens = 1000},
        .request = json::Json{{"model", "commander"}},
        .response = json::Json{{"choices", json::Json::array(
                                               {json::Json{{"content", "An article."}, {"reasoning", ""}}})}},
    };
}

class CallLogTest : public ::testing::Test {
protected:
    [[nodiscard]] CallLog& log() { return log_; }
    [[nodiscard]] db::Database& database() { return database_; }

private:
    testkit::TempDir dir_;
    db::Database database_ = unwrap(open_database(dir_ / "library.db"));
    CallLog log_{database_};
};

TEST_F(CallLogTest, KeepsEveryField) {
    const llm::CallRecord call = a_call("01K0000000000000000000000A", kNoon);
    ASSERT_OK(log().record(call));

    const auto calls = unwrap(log().list({}));
    ASSERT_EQ(calls.size(), 1U);
    const llm::CallRecord& kept = calls[0];
    EXPECT_EQ(kept.id, call.id);
    EXPECT_EQ(kept.started_at, call.started_at);
    EXPECT_EQ(kept.duration, call.duration);
    EXPECT_EQ(kept.first_token, call.first_token);
    EXPECT_EQ(kept.provider, "runtime");
    EXPECT_EQ(kept.model, "commander");
    EXPECT_EQ(kept.tag, call.tag);
    EXPECT_EQ(kept.request_key, call.request_key);
    EXPECT_FALSE(kept.error.has_value());
    EXPECT_EQ(kept.finish, llm::FinishReason::stop);
    EXPECT_EQ(kept.usage, call.usage);

    const CallPayload payload = unwrap(log().payload(call.id));
    EXPECT_EQ(payload.request, call.request);
    EXPECT_EQ(payload.response, call.response);
}

TEST_F(CallLogTest, KeepsFailuresAndMissingValues) {
    llm::CallRecord failed = a_call("01K0000000000000000000000B", kNoon);
    failed.error = Error{Errc::unavailable, "the engine is restarting"};
    failed.finish.reset();
    failed.first_token.reset();
    failed.usage = {};
    failed.tag = {.purpose = "bench"};
    failed.response = json::Json::object();
    ASSERT_OK(log().record(failed));

    const llm::CallRecord kept = unwrap(log().list({})).at(0);
    ASSERT_TRUE(kept.error.has_value());
    EXPECT_EQ(kept.error->code(), Errc::unavailable);
    EXPECT_THAT(kept.error->message(), HasSubstr("the engine is restarting"));
    EXPECT_FALSE(kept.finish.has_value());
    EXPECT_FALSE(kept.first_token.has_value());
    EXPECT_EQ(kept.tag.job_id, "") << "no job";
    EXPECT_EQ(
        unwrap(database().read([](db::ReadTx& tx) {
            return tx.query_int("SELECT count(*) FROM llm_calls WHERE job_id IS NULL AND mission_id IS NULL");
        })),
        1)
        << "stored as NULL, not as an empty name";
}

TEST_F(CallLogTest, ListsNewestFirstAndFilters) {
    llm::CallRecord bench = a_call("01K0000000000000000000000C", kNoon);
    bench.tag = {.purpose = "bench"};
    ASSERT_OK(log().record(bench));
    ASSERT_OK(log().record(a_call("01K0000000000000000000000D", kNoon + 1s)));
    llm::CallRecord other_job = a_call("01K0000000000000000000000E", kNoon + 2s);
    other_job.tag.job_id = "J2";
    ASSERT_OK(log().record(other_job));

    const auto all = unwrap(log().list({}));
    ASSERT_EQ(all.size(), 3U);
    EXPECT_EQ(all[0].id, other_job.id);
    EXPECT_EQ(all[2].id, bench.id);

    EXPECT_EQ(unwrap(log().list({.job_id = "J1"})).size(), 1U);
    EXPECT_EQ(unwrap(log().list({.purpose = "bench"})).at(0).id, bench.id);
    EXPECT_EQ(unwrap(log().list({.limit = 2})).size(), 2U);
}

TEST_F(CallLogTest, PruningDropsOldPayloadsAndKeepsTheAccounting) {
    ASSERT_OK(log().record(a_call("01K0000000000000000000000F", kNoon - 48h)));
    ASSERT_OK(log().record(a_call("01K0000000000000000000000G", kNoon)));
    EXPECT_EQ(unwrap(log().prune_payloads(kNoon - 24h)), 1);

    EXPECT_EQ(unwrap(log().list({})).size(), 2U) << "every call is still accounted for";
    EXPECT_ERRC(log().payload("01K0000000000000000000000F"), Errc::not_found);
    EXPECT_OK(log().payload("01K0000000000000000000000G"));
}

TEST_F(CallLogTest, OneCallIsRecordedOnce) {
    const llm::CallRecord call = a_call("01K0000000000000000000000H", kNoon);
    ASSERT_OK(log().record(call));
    EXPECT_ERRC(log().record(call), Errc::conflict);
    EXPECT_EQ(unwrap(log().list({})).size(), 1U) << "the failed insert left nothing behind";
}

}  // namespace
}  // namespace rr::library

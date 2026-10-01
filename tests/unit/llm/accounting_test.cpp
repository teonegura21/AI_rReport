#include <rr/llm/accounting.hpp>

#include <mutex>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>

#include "scripted_provider.hpp"

namespace rr::llm {
namespace {

using namespace std::chrono_literals;
using testing::answer;
using testing::ask;
using testing::ScriptedProvider;
using testkit::unwrap;

class MemoryRecorder final : public CallRecorder {
public:
    // Every record() fails from now on, as with a full disk.
    void refuse_all() {
        const std::lock_guard lock(mutex_);
        refuse_ = true;
    }

    [[nodiscard]] Result<void> record(const CallRecord& call) override {
        const std::lock_guard lock(mutex_);
        if (refuse_) {
            return fail(Errc::io, "disk full");
        }
        calls_.push_back(call);
        return ok();
    }

    [[nodiscard]] std::vector<CallRecord> calls() const {
        const std::lock_guard lock(mutex_);
        return calls_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<CallRecord> calls_;
    bool refuse_ = false;
};

TEST(AccountedProvider, RecordsEachCallWithItsCost) {
    FakeClock clock;
    ScriptedProvider inner{{answer("fine")}};
    inner.during_call = [&clock] { clock.advance(1500ms); };
    MemoryRecorder recorder;
    const AccountedProvider accounted{inner, recorder, clock};

    ChatRequest request = ask("hi");
    request.tag = {.purpose = "bench", .job_id = "J1"};
    const auto started = clock.now();
    const ChatResponse response = unwrap(accounted.chat(request, {}, {}));
    EXPECT_EQ(response.first().content, "fine");

    const std::vector<CallRecord> calls = recorder.calls();
    ASSERT_EQ(calls.size(), 1U);
    const CallRecord& call = calls[0];
    EXPECT_EQ(call.id.size(), 26U) << "a ULID";
    EXPECT_EQ(call.started_at, started);
    EXPECT_EQ(call.duration, 1500ms);
    EXPECT_EQ(call.provider, "scripted");
    EXPECT_EQ(call.model, "commander");
    EXPECT_EQ(call.tag, request.tag);
    EXPECT_EQ(call.request_key, request_key("scripted", request));
    EXPECT_FALSE(call.error.has_value());
    EXPECT_EQ(call.finish, FinishReason::stop);
    EXPECT_EQ(call.usage, response.usage);
    EXPECT_EQ(call.request, to_json(request));
    // Compared whole: a const operator[] on a missing key would abort the test program.
    EXPECT_EQ(
        call.response,
        (json::Json{{"choices", json::Json::array({json::Json{{"content", "fine"}, {"reasoning", ""}}})}}));
}

TEST(AccountedProvider, ACallOfManyChoicesIsRecordedAsTheLeastCompleteOfThem) {
    const FakeClock clock;
    ChatResponse drafts = testing::answers({"one", "two"});
    drafts.choices[1].finish = FinishReason::length;
    const ScriptedProvider inner{{drafts}};
    MemoryRecorder recorder;
    const AccountedProvider accounted{inner, recorder, clock};
    ChatRequest request = ask("two drafts");
    request.n = 2;
    ASSERT_OK(accounted.chat(request, {}, {}));

    const CallRecord call = recorder.calls().at(0);
    EXPECT_EQ(call.finish, FinishReason::length) << "a cut-short draft shows in the record";
    EXPECT_EQ(call.usage, drafts.usage);
    EXPECT_EQ(
        call.response,
        (json::Json{{"choices", json::Json::array({json::Json{{"content", "one"}, {"reasoning", ""}},
                                                   json::Json{{"content", "two"}, {"reasoning", ""}}})}}));
}

TEST(AccountedProvider, RecordsFailuresToo) {
    const FakeClock clock;
    const ScriptedProvider inner{{fail(Errc::unavailable, "engine restarting")}};
    MemoryRecorder recorder;
    const AccountedProvider accounted{inner, recorder, clock};
    EXPECT_ERRC(accounted.chat(ask("hi"), {}, {}), Errc::unavailable);

    const std::vector<CallRecord> calls = recorder.calls();
    ASSERT_EQ(calls.size(), 1U);
    ASSERT_TRUE(calls[0].error.has_value());
    EXPECT_EQ(calls[0].error->code(), Errc::unavailable);
    EXPECT_FALSE(calls[0].finish.has_value());
    EXPECT_EQ(calls[0].response, json::Json::object());
}

TEST(AccountedProvider, EachAttemptIsARecordSharingTheCallsKey) {
    const FakeClock clock;
    const ScriptedProvider inner{{fail(Errc::network, "reset"), answer("second time lucky")}};
    MemoryRecorder recorder;
    const AccountedProvider accounted{inner, recorder, clock};
    const RetryingProvider retrying{accounted, net::RetryPolicy{.max_attempts = 2},
                                    [](std::chrono::milliseconds, const std::stop_token&) { return true; }};
    ASSERT_OK(retrying.chat(ask("hi"), {}, {}));

    const std::vector<CallRecord> calls = recorder.calls();
    ASSERT_EQ(calls.size(), 2U);
    EXPECT_TRUE(calls[0].error.has_value());
    EXPECT_FALSE(calls[1].error.has_value());
    EXPECT_EQ(calls[0].request_key, calls[1].request_key);
    EXPECT_LT(calls[0].id, calls[1].id) << "ids sort by start";
}

TEST(AccountedProvider, AnAnswerOutlivesAFailureToRecordIt) {
    const FakeClock clock;
    const ScriptedProvider inner{{answer("already paid for")}};
    MemoryRecorder recorder;
    recorder.refuse_all();
    const AccountedProvider accounted{inner, recorder, clock};
    EXPECT_EQ(unwrap(accounted.chat(ask("hi"), {}, {})).first().content, "already paid for");
}

}  // namespace
}  // namespace rr::llm

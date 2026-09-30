#include <rr/llm/provider.hpp>

#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>

#include "scripted_provider.hpp"

namespace rr::llm {
namespace {

using testing::answer;
using testing::ask;
using ::testing::HasSubstr;
using testing::ScriptedProvider;
using testkit::unwrap;

// Retries without waiting.
const net::Pause kNoPause = [](std::chrono::milliseconds, const std::stop_token&) { return true; };

TEST(RetryingProvider, TriesTransientFailuresAgainAndRestartsTheStream) {
    const ScriptedProvider inner{{fail(Errc::network, "connection reset"),
                                  fail(Errc::unavailable, "engine restarting"), answer("done")}};
    const RetryingProvider retrying{inner, net::RetryPolicy{.max_attempts = 3}, kNoPause};

    std::vector<std::string> events;
    const StreamSink sink{.on_content = [&](std::string_view text) { events.emplace_back(text); },
                          .on_restart = [&] { events.emplace_back("<restart>"); }};
    const ChatResponse response = unwrap(retrying.chat(ask("hi"), sink, {}));
    EXPECT_EQ(response.content, "done");
    EXPECT_EQ(inner.requests().size(), 3U);
    EXPECT_EQ(events, (std::vector<std::string>{"<restart>", "<restart>", "done"}))
        << "every new attempt is announced before its text";
    EXPECT_EQ(retrying.name(), "scripted");
}

TEST(RetryingProvider, LeavesOtherFailuresAlone) {
    const ScriptedProvider inner{
        {fail(Errc::invalid_argument, "prompt longer than the context"), answer("x")}};
    const RetryingProvider retrying{inner, net::RetryPolicy{.max_attempts = 3}, kNoPause};
    EXPECT_ERRC(retrying.chat(ask("hi"), {}, {}), Errc::invalid_argument);
    EXPECT_EQ(inner.requests().size(), 1U);
}

TEST(RetryingProvider, GivesUpWhenTheAttemptsAreSpent) {
    const ScriptedProvider inner{{fail(Errc::timeout, "t1"), fail(Errc::timeout, "t2"), answer("late")}};
    const RetryingProvider retrying{inner, net::RetryPolicy{.max_attempts = 2}, kNoPause};
    const auto result = retrying.chat(ask("hi"), {}, {});
    ASSERT_ERRC(result, Errc::timeout);
    EXPECT_THAT(result.error().describe(), HasSubstr("gave up after 2 attempts"));
    EXPECT_EQ(inner.requests().size(), 2U);
}

}  // namespace
}  // namespace rr::llm

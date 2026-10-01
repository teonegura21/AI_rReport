#include <rr/library/call_memo.hpp>

#include <chrono>
#include <filesystem>

#include <gtest/gtest.h>

#include <rr/library/library.hpp>
#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

namespace rr::library {
namespace {

using namespace std::chrono_literals;
using testkit::unwrap;

// Two drafts from one call: everything a memo must give back.
llm::ChatResponse drafts() {
    return llm::ChatResponse{
        .choices = {llm::Choice{.content = "first draft",
                                .reasoning = "Start with the claim.",
                                .finish = llm::FinishReason::stop,
                                .completion_tokens = 7},
                    llm::Choice{.content = "second dra",
                                .finish = llm::FinishReason::length,
                                .completion_tokens = 9}},
        .usage = {.prompt_tokens = 120, .completion_tokens = 16, .cached_prompt_tokens = 100},
        .model = "commander",
        .first_token = 300ms,
        .duration = 2s};
}

class CallMemoTest : public ::testing::Test {
protected:
    [[nodiscard]] db::Database& database() { return database_; }
    [[nodiscard]] const FakeClock& clock() const { return clock_; }
    [[nodiscard]] std::filesystem::path file() const { return dir_ / "library.db"; }

private:
    testkit::TempDir dir_;
    db::Database database_ = unwrap(open_database(dir_ / "library.db"));
    FakeClock clock_;
};

TEST_F(CallMemoTest, KeepsAnAnswerWholeAndFindsItAgain) {
    CallMemo memo{database(), "J1", clock()};
    EXPECT_FALSE(unwrap(memo.find("k1")).has_value());
    ASSERT_OK(memo.keep("k1", drafts()));

    const auto found = unwrap(memo.find("k1"));
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->choices, drafts().choices) << "every choice, with its reasoning, ending and tokens";
    EXPECT_EQ(found->usage, drafts().usage);
    EXPECT_EQ(found->model, "commander");
    EXPECT_FALSE(found->first_token.has_value()) << "timings describe one run, and are not kept";
}

TEST_F(CallMemoTest, TheFirstAnswerStays) {
    CallMemo memo{database(), "J1", clock()};
    ASSERT_OK(memo.keep("k1", drafts()));
    llm::ChatResponse later = drafts();
    later.choices[0].content = "a later draft";
    ASSERT_OK(memo.keep("k1", later)) << "not an error: two threads may race to keep one answer";
    EXPECT_EQ(unwrap(memo.find("k1"))->choices[0].content, "first draft");
}

TEST_F(CallMemoTest, EachJobHasItsOwnMemory) {
    CallMemo first{database(), "J1", clock()};
    CallMemo second{database(), "J2", clock()};
    ASSERT_OK(first.keep("k1", drafts()));
    EXPECT_FALSE(unwrap(second.find("k1")).has_value()) << "the same question, another job";
    ASSERT_OK(second.keep("k1", drafts()));
    ASSERT_OK(first.keep("k2", drafts()));

    EXPECT_EQ(unwrap(first.forget()), 2);
    EXPECT_FALSE(unwrap(first.find("k1")).has_value());
    EXPECT_TRUE(unwrap(second.find("k1")).has_value()) << "forgetting one job leaves the others";
}

TEST_F(CallMemoTest, AnAnswerOutlivesTheProcessThatKeptIt) {
    {
        CallMemo memo{database(), "J1", clock()};
        ASSERT_OK(memo.keep("k1", drafts()));
    }
    db::Database reopened = unwrap(open_database(file()));
    const CallMemo memo{reopened, "J1", clock()};
    ASSERT_TRUE(unwrap(memo.find("k1")).has_value()) << "committed as it arrived: a resume finds it";
}

}  // namespace
}  // namespace rr::library

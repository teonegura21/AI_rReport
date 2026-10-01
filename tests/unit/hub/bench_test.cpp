// The bench's sums and words, without a pod: how a round of cache-check
// requests is summed up, what the verdict says, and how a level that failed
// is reported. tests/component/hub_cli_test.cpp runs the bench end to end.
#include <rr/hub/bench.hpp>

#include <chrono>
#include <cstdint>
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace rr::hub {
namespace {

using namespace std::chrono_literals;
using ::testing::HasSubstr;
using ::testing::Not;

llm::ChatResponse answered(std::int64_t prompt, std::int64_t cached, std::chrono::milliseconds first_token) {
    llm::ChatResponse response;
    response.usage.prompt_tokens = prompt;
    response.usage.cached_prompt_tokens = cached;
    response.first_token = first_token;
    return response;
}

TEST(BenchCache, ARoundIsAsGoodAsItsLeastReusedRequest) {
    const CacheRow row =
        cache_row({answered(220, 210, 3ms), answered(221, 0, 40ms), answered(220, 210, 4ms)});
    EXPECT_EQ(row.at_once, 3);
    EXPECT_EQ(row.prompt_tokens, 221) << "the largest prompt";
    EXPECT_EQ(row.cached_tokens, 0) << "one request read the whole prefix again: that is the news";
    EXPECT_EQ(row.first_token_p50, 4ms);
}

TEST(BenchCache, TheVerdictSaysWhereReuseStops) {
    CacheCheck check{.model = "m",
                     .first = {.prompt_tokens = 220, .cached_tokens = 0},
                     .next = {.prompt_tokens = 220, .cached_tokens = 210}};
    EXPECT_TRUE(check.reused());
    EXPECT_EQ(describe(check), "`m`: reused") << "one slot: no requests at once to send";

    check.together = CacheRow{.at_once = 4, .prompt_tokens = 220, .cached_tokens = 210};
    EXPECT_TRUE(check.reused());
    EXPECT_EQ(describe(check), "`m`: reused, also by 4 requests at once");

    check.together->cached_tokens = 0;
    EXPECT_FALSE(check.reused()) << "requests at once are how the pipeline runs";
    EXPECT_EQ(describe(check),
              "`m`: reused one request at a time, NOT by 4 at once: each of those reads the "
              "system message again");

    check.together.reset();
    check.next.cached_tokens = 110;
    EXPECT_TRUE(check.reused()) << "half the prompt is the threshold";
    check.next.cached_tokens = 109;
    EXPECT_FALSE(check.reused()) << "a few tokens in common (the template's start) are not the prefix";
    EXPECT_EQ(describe(check), "`m`: NOT reused: each request reads the whole system message again");
}

TEST(BenchReport, ALevelThatFailedSaysWhy) {
    BenchResult result{.started_at = "2026-10-01T00:00:00Z", .infantry_model = "squad"};
    result.infantry = {
        ParallelRow{.parallel = 1, .slots = 4, .tokens = 8, .total_tokens_per_second = 100.0},
        ParallelRow{.parallel = 2, .slots = 4, .failed = 2, .first_error = "HTTP 400: too long"},
        ParallelRow{.parallel = 8, .slots = 4, .failed = 3, .tokens = 40, .first_error = "HTTP 503: busy"}};
    const std::string text = to_markdown(result);
    EXPECT_THAT(text, HasSubstr("| 2 | 4 | – | – | – | 2 | – |")) << "no speeds where nothing ran";
    EXPECT_THAT(text, HasSubstr("- 2 at once: 2 of 2 failed: HTTP 400: too long"));
    EXPECT_THAT(text, HasSubstr("| 8 (queued) | 4 | "));
    EXPECT_THAT(text, HasSubstr("- 8 at once: 3 of 8 failed: HTTP 503: busy")) << "a partial failure too";
    EXPECT_THAT(text, Not(HasSubstr("- 1 at once"))) << "nothing to say where nothing failed";

    json::Json document = to_json(result);
    EXPECT_TRUE(document["infantry"]["rows"][0]["error"].is_null());
    EXPECT_EQ(document["infantry"]["rows"][1]["error"], "HTTP 400: too long");
}

}  // namespace
}  // namespace rr::hub

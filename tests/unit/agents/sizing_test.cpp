#include <rr/agents/sizing.hpp>

#include <string>

#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>

namespace rr::agents {
namespace {

using testkit::unwrap;

constexpr std::int64_t kMarkers = prompt::TokenEstimator::kTokensPerMessage;

TEST(Sizing, APromptIsItsMessagesAndTheirMarkers) {
    const prompt::TokenEstimator estimator{0.25};
    llm::ChatRequest request{
        .messages = {llm::Message::system(std::string(40, 's')), llm::Message::user(std::string(10, 'u'))}};
    EXPECT_EQ(unwrap(prompt_tokens(request, estimator)), 10 + 3 + (2 * kMarkers)) << "each text rounds up";

    EXPECT_ERRC(footprint_of(request, estimator), Errc::invalid_argument) << "no max_tokens, no size";
    request.max_tokens = 100;
    EXPECT_EQ(unwrap(footprint_of(request, estimator)),
              (Footprint{.prompt = 10 + 3 + (2 * kMarkers), .answer = 100}));

    request.messages.back().content.push_back(
        llm::ContentPart{.kind = llm::ContentPart::Kind::image_url, .value = "data:image/png;base64,AA=="});
    EXPECT_ERRC(prompt_tokens(request, estimator), Errc::unsupported) << "an image's tokens are unknown";
}

TEST(Sizing, EachSlotOfASplitEngineHoldsOneSequence) {
    const Engine engine{.slots = 4, .context = 16384, .ctx_per_slot = 4096};
    EXPECT_EQ(choices_that_fit(engine, {.prompt = 3000, .answer = 1096}), 4) << "exactly a slot";
    EXPECT_EQ(choices_that_fit(engine, {.prompt = 3000, .answer = 1097}), 0);
    EXPECT_EQ(choices_that_fit(engine, {.prompt = 100, .answer = 0}), 0) << "an answer needs a token";
    EXPECT_EQ(reservation(engine, {.prompt = 3000, .answer = 1000}, 4), 0) << "nothing shared to reserve";
}

TEST(Sizing, ASharedPoolKeepsThePromptOnceAndEachAnswersRoom) {
    const Engine engine{.slots = 4, .context = 10000, .ctx_per_slot = 10000, .kv_unified = true};
    EXPECT_EQ(choices_that_fit(engine, {.prompt = 4000, .answer = 2000}), 3) << "4000 + 3 × 2000 = 10000";
    EXPECT_EQ(choices_that_fit(engine, {.prompt = 1000, .answer = 1000}), 4) << "never more than its slots";
    EXPECT_EQ(choices_that_fit(engine, {.prompt = 9000, .answer = 1001}), 0);
    EXPECT_EQ(reservation(engine, {.prompt = 4000, .answer = 2000}, 3), 10000);
    EXPECT_EQ(reservation(engine, {.prompt = 4000, .answer = 2000}, 1), 6000);

    // The model's trained context can cap a sequence below the pool.
    const Engine capped{.slots = 4, .context = 65536, .ctx_per_slot = 8192, .kv_unified = true};
    EXPECT_EQ(choices_that_fit(capped, {.prompt = 6000, .answer = 2193}), 0);
    EXPECT_EQ(choices_that_fit(capped, {.prompt = 6000, .answer = 2192}), 4);
}

}  // namespace
}  // namespace rr::agents

#include <rr/prompt/tokens.hpp>

#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>

namespace rr::prompt {
namespace {

using testkit::unwrap;

TEST(TokenEstimator, CountsCharactersAndRoundsUp) {
    const TokenEstimator estimator{0.25};
    EXPECT_EQ(estimator.estimate(""), 0);
    EXPECT_EQ(estimator.estimate("abcd"), 1);
    EXPECT_EQ(estimator.estimate("abcde"), 2) << "a part of a token is a token";
    EXPECT_EQ(estimator.estimate(Rendered{.system = "abcd", .user = "abcdefgh"}),
              1 + 2 + (2 * TokenEstimator::kTokensPerMessage))
        << "each message adds its template markers";
}

TEST(TokenEstimator, ByDefaultItOverestimatesEnglish) {
    // About four characters a token is typical; the default assumes three.
    const std::string prose(4000, 'x');
    EXPECT_GE(TokenEstimator{}.estimate(prose), 1000);
}

TEST(TokenEstimator, APromptFitsWithItsAnswerOrNot) {
    const TokenEstimator estimator{1.0};
    const Rendered prompt{.system = std::string(100, 's'), .user = std::string(84, 'u')};
    const std::int64_t needed = 100 + 84 + (2 * TokenEstimator::kTokensPerMessage);  // 200
    EXPECT_TRUE(estimator.fits(prompt, 56, needed + 56));
    EXPECT_FALSE(estimator.fits(prompt, 57, needed + 56)) << "the answer needs room too";
}

TEST(TokenEstimator, CalibrationTakesAHighQuantileOfWhatWasSeen) {
    // Ten calls: 0.20, 0.21, … 0.29 tokens per character.
    std::vector<Observation> calls;
    calls.reserve(30);
    for (int index = 0; index < 10; ++index) {
        calls.push_back({.characters = 1000, .tokens = 200 + (10 * index)});
    }
    EXPECT_DOUBLE_EQ(unwrap(calibrate(calls)).tokens_per_character(), 0.28)
        << "9 of 10 at or under: the ninth by rank";
    EXPECT_DOUBLE_EQ(unwrap(calibrate(calls, 1.0)).tokens_per_character(), 0.29);
    EXPECT_DOUBLE_EQ(unwrap(calibrate(calls, 0.5)).tokens_per_character(), 0.24);

    for (int index = 0; index < 10; ++index) {
        calls.push_back({.characters = 0, .tokens = 50});    // no prompt text recorded
        calls.push_back({.characters = 1000, .tokens = 0});  // no count reported
    }
    EXPECT_DOUBLE_EQ(unwrap(calibrate(calls)).tokens_per_character(), 0.28)
        << "unusable records are left out, not counted as zero";

    // The rank rounds up: of three, the 0.9 quantile is the largest.
    const std::vector<Observation> three{
        {.characters = 10, .tokens = 1}, {.characters = 10, .tokens = 2}, {.characters = 10, .tokens = 3}};
    EXPECT_DOUBLE_EQ(unwrap(calibrate(three)).tokens_per_character(), 0.3);
}

TEST(TokenEstimator, CalibrationNeedsSomethingToGoOn) {
    EXPECT_ERRC(calibrate({}), Errc::invalid_argument);
    const std::vector<Observation> one{{.characters = 100, .tokens = 25}};
    EXPECT_ERRC(calibrate(one, 0.4), Errc::invalid_argument) << "below the median is no safety margin";
    EXPECT_ERRC(calibrate(one, 1.5), Errc::invalid_argument);
    EXPECT_ERRC(calibrate(one, std::nan("")), Errc::invalid_argument) << "not a number is not a quantile";
    EXPECT_DOUBLE_EQ(unwrap(calibrate(one)).tokens_per_character(), 0.25);
}

}  // namespace
}  // namespace rr::prompt

#include <rr/reason/vote.hpp>

#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>

namespace rr::reason {
namespace {

using testkit::unwrap;

const std::vector<std::string> kSentences{"S1", "S2", "S3"};

// Three samples labelling three sentences.
std::vector<Ballot> three_samples() {
    return {
        {.item = "S1", .value = "claim", .text = "Water boils at 100 °C.", .sample = 0},
        {.item = "S2", .value = "aside", .sample = 0},
        {.item = "S3", .value = "claim", .sample = 0},
        {.item = "S1", .value = "claim", .text = "Water boils at 100 °C at sea level.", .sample = 1},
        {.item = "S2", .value = "claim", .sample = 1},
        {.item = "S3", .value = "joke", .sample = 1},
        {.item = "S1", .value = "claim", .text = "Water boils at sea level.", .sample = 2},
        {.item = "S2", .value = "aside", .sample = 2},
        {.item = "S3", .value = "aside", .sample = 2},
    };
}

TEST(Vote, CountsVotesPerItem) {
    const std::vector<Consensus> votes = unwrap(vote_keyed(kSentences, three_samples(), 0.66));
    ASSERT_EQ(votes.size(), 3U);

    EXPECT_EQ(votes[0].item, "S1");
    EXPECT_EQ(votes[0].votes, 3);
    EXPECT_EQ(votes[0].counts, (std::vector<Count>{{.value = "claim", .votes = 3}}));
    EXPECT_EQ(votes[0].winner, "claim");

    EXPECT_EQ(votes[1].counts,
              (std::vector<Count>{{.value = "aside", .votes = 2}, {.value = "claim", .votes = 1}}));
    EXPECT_EQ(votes[1].winner, "aside") << "two of three reach 0.66";

    EXPECT_EQ(votes[2].counts, (std::vector<Count>{{.value = "claim", .votes = 1},
                                                   {.value = "joke", .votes = 1},
                                                   {.value = "aside", .votes = 1}}))
        << "ties keep the order first voted";
    EXPECT_FALSE(votes[2].winner.has_value()) << "three ways apart: contested";
    EXPECT_FALSE(votes[2].medoid.has_value());
}

TEST(Vote, TheWinnerNeedNotBeVotedFirst) {
    const std::vector<Ballot> late{
        {.item = "S1", .value = "aside", .sample = 0},
        {.item = "S1", .value = "claim", .sample = 1},
        {.item = "S1", .value = "claim", .sample = 2},
    };
    const Consensus vote = unwrap(vote_keyed({kSentences.data(), 1}, late, 0.66))[0];
    EXPECT_EQ(vote.counts,
              (std::vector<Count>{{.value = "claim", .votes = 2}, {.value = "aside", .votes = 1}}));
    EXPECT_EQ(vote.winner, "claim");
}

TEST(Vote, AQuorumIsAShareOfTheVotesCast) {
    const std::vector<Ballot> two_of_three{
        {.item = "S1", .value = "claim", .sample = 0},
        {.item = "S1", .value = "claim", .sample = 1},
        {.item = "S1", .value = "aside", .sample = 2},
    };
    EXPECT_TRUE(unwrap(vote_keyed({kSentences.data(), 1}, two_of_three, 0.66))[0].winner.has_value());
    EXPECT_FALSE(unwrap(vote_keyed({kSentences.data(), 1}, two_of_three, 0.67))[0].winner.has_value())
        << "0.67 is more than two of three";
    EXPECT_FALSE(unwrap(vote_keyed({kSentences.data(), 1}, two_of_three, 1.0))[0].winner.has_value());

    // Fourteen of 25 is 0.56 exactly, though 0.56 × 25 is 14.000000000000002
    // in floating point.
    std::vector<Ballot> fourteen_of_25;
    fourteen_of_25.reserve(25);
    for (int sample = 0; sample < 25; ++sample) {
        fourteen_of_25.push_back({.item = "S1", .value = sample < 14 ? "claim" : "aside", .sample = sample});
    }
    EXPECT_TRUE(unwrap(vote_keyed({kSentences.data(), 1}, fourteen_of_25, 0.56))[0].winner.has_value());
}

TEST(Vote, ATieIsNeverSettled) {
    const std::vector<Count> tie{{.value = "claim", .votes = 2}, {.value = "aside", .votes = 2}};
    EXPECT_FALSE(settled(tie, 0.5)) << "the order the samples came in must not decide";
    EXPECT_FALSE(settled({}, 0.5)) << "no vote, no answer";
    const std::vector<Count> alone{{.value = "claim", .votes = 1}};
    EXPECT_TRUE(settled(alone, 1.0));
}

TEST(Vote, AnItemNobodyVotedOnIsContested) {
    const std::vector<Ballot> partial{{.item = "S1", .value = "claim", .sample = 0}};
    const std::vector<Consensus> votes = unwrap(vote_keyed(kSentences, partial, 0.5));
    EXPECT_EQ(votes[1].item, "S2");
    EXPECT_EQ(votes[1].votes, 0);
    EXPECT_FALSE(votes[1].winner.has_value());
}

TEST(Vote, TheMedoidIsTheWinnersMostTypicalText) {
    const std::vector<Consensus> votes = unwrap(vote_keyed(kSentences, three_samples(), 0.66));
    EXPECT_EQ(votes[0].medoid, 1) << "the text closest to both others";

    // Only the winner's ballots compete.
    const std::vector<Ballot> ballots{
        {.item = "S1", .value = "aside", .text = "water boils at 100 at sea level", .sample = 0},
        {.item = "S1", .value = "claim", .text = "boils at 100", .sample = 1},
        {.item = "S1", .value = "claim", .text = "water boils at 100", .sample = 2},
        {.item = "S1", .value = "claim", .text = "water boils at 100 at sea level", .sample = 3},
    };
    EXPECT_EQ(unwrap(vote_keyed({kSentences.data(), 1}, ballots, 0.5))[0].medoid, 2);
}

// One sample of each sentence, each as sure as the probability of its
// tokens says: S1 sure, S2 not, S3 not known.
std::vector<Ballot> lone_samples() {
    return {{.item = "S1", .value = "claim", .sample = 0, .confidence = 0.95},
            {.item = "S2", .value = "aside", .sample = 0, .confidence = 0.5},
            {.item = "S3", .value = "claim", .sample = 0}};
}

TEST(Vote, ALoneSampleSettlesItsItemOnlyWhenSure) {
    const std::vector<Consensus> votes = unwrap(vote_keyed(kSentences, lone_samples(), 0.66, 0.9));
    EXPECT_EQ(votes[0].winner, "claim") << "0.95 reaches 0.9";
    EXPECT_EQ(votes[0].medoid, 0);
    EXPECT_FALSE(votes[1].winner.has_value()) << "0.5 does not";
    EXPECT_EQ(votes[1].counts, (std::vector<Count>{{.value = "aside", .votes = 1}}))
        << "counted all the same";
    EXPECT_FALSE(votes[2].winner.has_value()) << "a confidence not known is no confidence";

    const std::vector<Ballot> at_the_bar{{.item = "S1", .value = "claim", .sample = 0, .confidence = 0.9}};
    EXPECT_TRUE(unwrap(vote_keyed({kSentences.data(), 1}, at_the_bar, 0.66, 0.9))[0].winner.has_value())
        << "the bar is reached at it";
    // 0.3 × 3 is 0.8999999999999999 in floating point: the vote's slack keeps it at the bar.
    const std::vector<Ballot> a_hair_under{
        {.item = "S1", .value = "claim", .sample = 0, .confidence = 0.3 * 3}};
    EXPECT_TRUE(unwrap(vote_keyed({kSentences.data(), 1}, a_hair_under, 0.66, 0.9))[0].winner.has_value());

    for (const Consensus& vote : unwrap(vote_keyed(kSentences, lone_samples(), 0.66))) {
        EXPECT_TRUE(vote.winner.has_value()) << vote.item << ": sure 0, every lone sample settles, as before";
    }
}

TEST(Vote, SamplesThatAgreeSettleTheirItemHoweverUnsureEachWas) {
    const std::vector<Ballot> two{{.item = "S1", .value = "claim", .sample = 0, .confidence = 0.1},
                                  {.item = "S1", .value = "claim", .sample = 1}};
    EXPECT_EQ(unwrap(vote_keyed({kSentences.data(), 1}, two, 0.66, 0.9))[0].winner, "claim")
        << "agreement is evidence a sample's own probability is not";
}

TEST(Vote, RefusesBallotsItCannotCount) {
    const std::vector<Ballot> stray{{.item = "S9", .value = "claim", .sample = 0}};
    EXPECT_ERRC(vote_keyed(kSentences, stray, 0.5), Errc::invalid_argument) << "an item not voted on";
    const std::vector<Ballot> twice{{.item = "S1", .value = "claim", .sample = 0},
                                    {.item = "S1", .value = "aside", .sample = 0}};
    EXPECT_ERRC(vote_keyed(kSentences, twice, 0.5), Errc::invalid_argument) << "one sample, one vote";
    const std::vector<std::string> doubled{"S1", "S1"};
    EXPECT_ERRC(vote_keyed(doubled, {}, 0.5), Errc::invalid_argument);
}

TEST(Waves, DrawMoreOnlyWhereItCanHelp) {
    const Waves waves{.first = 3, .step = 2, .max = 7, .quorum = 0.66};
    std::vector<Consensus> votes = unwrap(vote_keyed(kSentences, three_samples(), waves.quorum));
    EXPECT_EQ(next_wave(votes, 3, waves), 2) << "S3 is contested, and four more votes could settle it";
    EXPECT_EQ(next_wave(votes, 7, waves), 0) << "max is reached";
    const Waves one_left{.first = 3, .step = 2, .max = 4, .quorum = 0.5};
    EXPECT_EQ(next_wave(votes, 3, one_left), 1) << "never past max";

    votes.pop_back();  // S1 and S2 are settled
    EXPECT_EQ(next_wave(votes, 3, waves), 0) << "nothing left to settle";

    // 1–1–1 with one sample to come can never reach two thirds.
    const Waves one_more{.first = 3, .step = 1, .max = 4, .quorum = 0.66};
    const std::vector<Consensus> three_ways =
        unwrap(vote_keyed(kSentences, three_samples(), one_more.quorum));
    EXPECT_EQ(next_wave(three_ways, 3, one_more), 0) << "futile: 2 of 4 is not two thirds";

    EXPECT_EQ(next_wave(three_ways, 3, Waves{.first = 3, .step = 0, .max = 3, .quorum = 0.66}), 0);
}

TEST(Waves, AnUnsureLoneSampleDrawsASecond) {
    const Waves waves{.first = 1, .step = 1, .max = 3, .quorum = 0.66, .sure = 0.9};
    const std::vector<Consensus> votes =
        unwrap(vote_keyed(kSentences, lone_samples(), waves.quorum, waves.sure));
    EXPECT_EQ(next_wave(votes, 1, waves), 1) << "S2 and S3 were unsure: a second sample could settle them";

    std::vector<Ballot> sure = lone_samples();
    for (Ballot& ballot : sure) {
        ballot.confidence = 1.0;
    }
    const std::vector<Consensus> settled = unwrap(vote_keyed(kSentences, sure, waves.quorum, waves.sure));
    EXPECT_EQ(next_wave(settled, 1, waves), 0) << "every lone sample was sure: one each is enough";
}

TEST(Waves, AreChecked) {
    EXPECT_TRUE(waves_problems(Waves{.first = 3, .step = 2, .max = 7, .quorum = 0.66}).empty());
    EXPECT_TRUE(waves_problems(Waves{}).empty());
    EXPECT_EQ(waves_problems(Waves{.first = 0}).size(), 2U) << "no first sample, and a max no wave reaches";
    EXPECT_EQ(waves_problems(Waves{.first = 3, .step = 0, .max = 5}).size(), 1U)
        << "nothing would draw the rest";
    EXPECT_EQ(waves_problems(Waves{.first = 3, .step = 1, .max = 2}).size(), 1U) << "max below first";
    EXPECT_EQ(waves_problems(Waves{.step = -1}).size(), 1U);
    EXPECT_EQ(waves_problems(Waves{.quorum = 0.0}).size(), 1U);
    EXPECT_EQ(waves_problems(Waves{.quorum = std::nan("")}).size(), 1U);
    EXPECT_TRUE(waves_problems(Waves{.sure = 0.0}).empty()) << "0: any lone sample settles its item";
    EXPECT_TRUE(waves_problems(Waves{.sure = 1.0}).empty());
    EXPECT_EQ(waves_problems(Waves{.sure = 1.5}).size(), 1U);
    EXPECT_EQ(waves_problems(Waves{.sure = -0.1}).size(), 1U);
    EXPECT_EQ(waves_problems(Waves{.sure = std::nan("")}).size(), 1U);
}

TEST(Medoid, PicksTheTextClosestToAllOthers) {
    EXPECT_FALSE(medoid({}).has_value());
    const std::vector<std::string> one{"anything"};
    EXPECT_EQ(medoid(one), 0U);
    const std::vector<std::string> texts{"a b c", "a b c d", "x y z"};
    EXPECT_EQ(medoid(texts), 0U);
    const std::vector<std::string> tied{"a b", "a b"};
    EXPECT_EQ(medoid(tied), 0U) << "a tie goes to the first";
    const auto length = [](std::string_view lhs, std::string_view rhs) {
        return static_cast<double>(lhs.size() > rhs.size() ? lhs.size() - rhs.size()
                                                           : rhs.size() - lhs.size());
    };
    const std::vector<std::string> sized{"a", "aaaa", "aa"};
    EXPECT_EQ(medoid(sized, length), 2U) << "any distance";
}

TEST(Medoid, WordsAreLowercasedAndUnicodeKept) {
    EXPECT_DOUBLE_EQ(word_distance("Water boils.", "water BOILS"), 0.0);
    EXPECT_DOUBLE_EQ(word_distance("", ""), 0.0);
    EXPECT_DOUBLE_EQ(word_distance("a", ""), 1.0);
    EXPECT_DOUBLE_EQ(word_distance("a b", "b c"), 1.0 - (1.0 / 3.0));
    EXPECT_DOUBLE_EQ(word_distance("café crème", "café"), 0.5) << "UTF-8 letters are letters";
}

}  // namespace
}  // namespace rr::reason

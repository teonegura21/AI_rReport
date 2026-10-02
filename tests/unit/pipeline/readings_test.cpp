// What models made of a window, sentence by sentence: an answer read, the
// readings voted on, the base reading found, the stretches it does not
// settle, a stretch read again, how much of a sentence its atoms hold.
#include <rr/pipeline/readings.hpp>

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <rr/core/error.hpp>
#include <rr/testkit/result.hpp>

namespace rr::pipeline {
namespace {

using knowledge::AtomId;
using knowledge::AtomKind;
using knowledge::Filler;
using knowledge::FillerLabel;
using knowledge::SentenceId;
using testkit::unwrap;

// An atom as an atomize answer writes it: every field the schema asks for.
json::Json atom(std::string text, std::string kind = "claim", std::string modality = "asserted",
                std::vector<std::string> also = {}, json::Json must_quote = nullptr) {
    return json::Json{{"also", std::move(also)},
                      {"names", json::Json::array()},
                      {"terms", json::Json::array()},
                      {"text", std::move(text)},
                      {"kind", std::move(kind)},
                      {"modality", std::move(modality)},
                      {"register", "main"},
                      {"intensity", "medium"},
                      {"speaker", nullptr},
                      {"attributed_to", nullptr},
                      {"must_quote", std::move(must_quote)}};
}

json::Json row(std::string sentence, json::Json filler, std::vector<json::Json> atoms = {}) {
    json::Json written = json::Json::array();
    for (json::Json& item : atoms) {
        written.push_back(std::move(item));
    }
    return json::Json{
        {"sentence", std::move(sentence)}, {"filler", std::move(filler)}, {"atoms", std::move(written)}};
}

Reading read(std::vector<json::Json> rows) {
    json::Json sentences = json::Json::array();
    for (json::Json& item : rows) {
        sentences.push_back(std::move(item));
    }
    return unwrap(reading_of(json::Json{{"sentences", std::move(sentences)}}));
}

std::vector<SentenceId> ids(std::uint32_t first, std::uint32_t last) {
    std::vector<SentenceId> all;
    for (std::uint32_t number = first; number <= last; ++number) {
        all.emplace_back(number);
    }
    return all;
}

// A reading that makes each of S1… a claim but where `other` says.
Reading claims(std::size_t count, const std::vector<std::pair<std::uint32_t, std::string>>& other = {}) {
    std::vector<json::Json> rows;
    for (std::uint32_t number = 1; number <= count; ++number) {
        std::string kind = "claim";
        for (const auto& [sentence, instead] : other) {
            kind = sentence == number ? instead : kind;
        }
        rows.push_back(
            row(std::format("S{}", number), nullptr, {atom(std::format("Said {}.", number), kind)}));
    }
    return read(std::move(rows));
}

TEST(Readings, AnAnswerIsReadAsTheLedgerWouldBe) {
    const Reading reading = read({row("S1", "greeting"),
                                  row("S2", nullptr,
                                      {atom("Radium glows."), atom("It glows for years.", "fact", "asserted",
                                                                   {"S3", "S2"}, "it glows for years")}),
                                  row("S3", nullptr)});
    ASSERT_EQ(reading.atoms.size(), 2U);
    EXPECT_EQ(reading.atoms[0].id, AtomId{1});
    EXPECT_EQ(reading.atoms[0].sentences, ids(2, 2));
    EXPECT_EQ(reading.atoms[0].text, "Radium glows.");
    EXPECT_EQ(reading.atoms[1].id, AtomId{2});
    EXPECT_EQ(reading.atoms[1].sentences, ids(2, 3))
        << "its row's sentence and its also, in order, once each";
    EXPECT_EQ(reading.atoms[1].kind, AtomKind::fact);
    EXPECT_TRUE(reading.atoms[1].must_quote);
    EXPECT_EQ(reading.atoms[1].quote, "it glows for years");
    EXPECT_EQ(reading.fillers,
              (std::vector<Filler>{Filler{.sentence = SentenceId{1}, .label = FillerLabel::greeting}}));

    const Result<Reading> twice = reading_of(
        json::Json{{"sentences", json::Json::array({row("S1", "greeting"), row("S1", "disfluency")})}});
    ASSERT_FALSE(twice.has_value()) << "two labels for one sentence";
    EXPECT_EQ(twice.error().code(), Errc::invalid_argument);
}

TEST(Readings, AVerdictIsWhatASentenceBecameNotItsWords) {
    const Reading reading = read({row("S1", "greeting"),
                                  row("S2", nullptr,
                                      {atom("Radium glows."), atom("It may glow for years.", "number",
                                                                   "hedged", {"S3"}, "glow for years")}),
                                  row("S3", nullptr)});
    EXPECT_EQ(verdict_on(reading, SentenceId{1}), "filler greeting");
    EXPECT_EQ(verdict_on(reading, SentenceId{2}), "atoms claim/asserted, number/hedged/quote") << "sorted";
    EXPECT_EQ(verdict_on(reading, SentenceId{3}), "atoms number/hedged/quote") << "an atom also citing it";
    EXPECT_EQ(verdict_on(reading, SentenceId{4}), "") << "nothing made of it";

    json::Json other_words = atom("Radium shines.");
    other_words["register"] = "aside";
    other_words["intensity"] = "high";
    EXPECT_EQ(verdict_on(read({row("S2", nullptr, {other_words})}), SentenceId{2}), "atoms claim/asserted")
        << "words, register and intensity are not voted on";
    EXPECT_EQ(
        verdict_on(
            read({row("S2", nullptr, {atom("It may glow.", "number", "hedged"), atom("Radium glows.")})}),
            SentenceId{2}),
        "atoms claim/asserted, number/hedged")
        << "in any order written";
}

TEST(Readings, BallotsAreOneASentenceASample) {
    const std::vector<Reading> readings{claims(2), claims(2, {{2, "fact"}})};
    const std::vector<SentenceId> sentences = ids(1, 2);
    const std::vector<reason::Ballot> ballots = ballots_of(readings, sentences);
    ASSERT_EQ(ballots.size(), 4U);
    EXPECT_EQ(ballots[3].item, "S2");
    EXPECT_EQ(ballots[3].sample, 1);
    EXPECT_EQ(ballots[3].value, "atoms fact/asserted");
    EXPECT_EQ(ballots[3].text, "Said 2.");
}

TEST(Readings, TheBaseReadingWinsTheMostVotes) {
    // S2 is a fact to two readings of three, S3 a claim: only the third
    // agrees with both.
    const std::vector<Reading> readings{claims(3), claims(3, {{2, "fact"}, {3, "fact"}}),
                                        claims(3, {{2, "fact"}})};
    const std::vector<SentenceId> sentences = ids(1, 3);
    std::vector<std::string> items;
    items.reserve(sentences.size());
    for (const SentenceId id : sentences) {
        items.push_back(id.str());
    }
    const std::vector<reason::Consensus> votes =
        unwrap(reason::vote_keyed(items, ballots_of(readings, sentences), 0.66));
    EXPECT_EQ(base_of(readings, sentences, votes), 2U);

    const std::vector<Reading> two{claims(3), claims(3, {{2, "fact"}})};
    const std::vector<reason::Consensus> tied =
        unwrap(reason::vote_keyed(items, ballots_of(two, sentences), 0.66));
    EXPECT_EQ(base_of(two, sentences, tied), 0U) << "the first on a tie";
}

// Votes on S1… won by `winners`; none where a winner is empty.
std::vector<reason::Consensus> won(const std::vector<std::string>& winners) {
    std::vector<reason::Consensus> votes;
    votes.reserve(winners.size());
    for (std::size_t index = 0; index < winners.size(); ++index) {
        votes.push_back(reason::Consensus{
            .item = std::format("S{}", index + 1),
            .winner = winners[index].empty() ? std::nullopt : std::optional{winners[index]}});
    }
    return votes;
}

TEST(Readings, WhatTheBaseDoesNotSettleIsGrownUntilNoAtomCrossesItsEdge) {
    // The base: S1 a claim, S2–S3 one fact, S4–S6 claims.
    const Reading base = read({row("S1", nullptr, {atom("One.")}),
                               row("S2", nullptr, {atom("Two and three.", "fact", "asserted", {"S3"})}),
                               row("S3", nullptr), row("S4", nullptr, {atom("Four.")}),
                               row("S5", nullptr, {atom("Five.")}), row("S6", nullptr, {atom("Six.")})});
    const std::vector<SentenceId> sentences = ids(1, 6);
    const std::string claim = "atoms claim/asserted";
    const std::string fact = "atoms fact/asserted";
    EXPECT_TRUE(unsettled(base, sentences, won({claim, fact, fact, claim, claim, claim})).empty());
    EXPECT_EQ(unsettled(base, sentences,
                        won({claim, fact, "filler disfluency", claim, "", "atoms question/question"})),
              (std::vector<Stretch>{{.begin = 1, .end = 3}, {.begin = 4, .end = 6}}))
        << "S3 out-voted takes S2 with it, whose atom it shares; S5 tied and S6 out-voted are one stretch";
    EXPECT_EQ(unsettled(base, sentences, won({claim, fact, "", "", claim, claim})),
              (std::vector<Stretch>{{.begin = 1, .end = 4}}))
        << "grown, then joined with the stretch it touches";

    // An atom of S1–S3, written first, and one of S3–S5: S5 out-voted grows
    // over the second, then the first.
    const Reading chained = read(
        {row("S1", nullptr, {atom("One to three.", "claim", "asserted", {"S2", "S3"})}), row("S2", nullptr),
         row("S3", nullptr, {atom("Three to five.", "claim", "asserted", {"S4", "S5"})}), row("S4", nullptr),
         row("S5", nullptr)});
    const std::string both = "atoms claim/asserted, claim/asserted";  // S3, which both cite
    EXPECT_EQ(unsettled(chained, ids(1, 5), won({claim, claim, both, claim, fact})),
              (std::vector<Stretch>{{.begin = 0, .end = 5}}))
        << "grown until no atom crosses its edge, however many passes that takes";
}

TEST(Readings, ASettledReadingReplacesTheBasesOnItsStretch) {
    const Reading base = read({row("S1", nullptr, {atom("One.")}), row("S2", "repetition"),
                               row("S3", nullptr, {atom("Three.", "fact")}), row("S4", "decorative")});
    const Reading again = read({row("S2", nullptr, {atom("Two tons.", "number")}), row("S3", "disfluency")});
    const Reading settled = replaced(base, ids(1, 4), Stretch{.begin = 1, .end = 3}, again);
    ASSERT_EQ(settled.atoms.size(), 2U);
    EXPECT_EQ(settled.atoms[0].text, "One.");
    EXPECT_EQ(settled.atoms[1].text, "Two tons.");
    EXPECT_EQ(settled.atoms[1].id, AtomId{2}) << "renumbered";
    EXPECT_EQ(settled.fillers,
              (std::vector<Filler>{Filler{.sentence = SentenceId{3}, .label = FillerLabel::disfluency},
                                   Filler{.sentence = SentenceId{4}, .label = FillerLabel::decorative}}))
        << "S2's label and S3's atom gone with the base's reading of them; in the order of their sentences";
}

TEST(Readings, CoverageIsTheShareOfASentencesWordsItsAtomsHold) {
    const knowledge::Sentence sentence{.id = SentenceId{1},
                                       .text = "Marie Curie measured 7 samples in 1902.",
                                       .span = unwrap(TimeSpan::make(Millis{0}, Millis{4'000}))};
    knowledge::Atom held{.id = AtomId{1}, .text = "Curie measured samples.", .sentences = ids(1, 1)};
    EXPECT_DOUBLE_EQ(coverage(sentence, std::vector{held}), 3.0 / 7.0);
    held.names = {"Marie Curie"};
    EXPECT_DOUBLE_EQ(coverage(sentence, std::vector{held}), 4.0 / 7.0) << "its names count";
    held.must_quote = true;
    held.quote = "7 samples in 1902";
    EXPECT_DOUBLE_EQ(coverage(sentence, std::vector{held}), 1.0) << "and its quote";
    const knowledge::Atom termed{
        .id = AtomId{1}, .text = "Measured.", .sentences = ids(1, 1), .terms = {"7 samples"}};
    EXPECT_DOUBLE_EQ(coverage(sentence, std::vector{termed}), 3.0 / 7.0) << "and its terms";
    EXPECT_DOUBLE_EQ(coverage(sentence, {}), 0.0);
    knowledge::Sentence silent = sentence;
    silent.text = "\u2026";
    EXPECT_DOUBLE_EQ(coverage(silent, {}), 1.0) << "a sentence without a word lacks nothing";
}

TEST(Readings, WindowsJoinIntoOneLedgerInTheOrderSaid) {
    const Reading first =
        read({row("S1", nullptr, {atom("One.")}), row("S2", nullptr, {atom("Two.")}), row("S3", "greeting")});
    const Reading second = read({row("S4", "disfluency"), row("S5", nullptr, {atom("Five.")})});
    const knowledge::AtomLedger ledger = ledger_of(std::vector{first, second});
    ASSERT_EQ(ledger.atoms.size(), 3U);
    EXPECT_EQ(ledger.atoms[2].id, AtomId{3});
    EXPECT_EQ(ledger.atoms[2].text, "Five.");
    EXPECT_EQ(ledger.fillers,
              (std::vector<Filler>{Filler{.sentence = SentenceId{3}, .label = FillerLabel::greeting},
                                   Filler{.sentence = SentenceId{4}, .label = FillerLabel::disfluency}}));
}

}  // namespace
}  // namespace rr::pipeline

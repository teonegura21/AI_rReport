// Silver annotation, its pure parts: windows sized to their engines, drafts
// checked against the words said, joined, and kept.
#include <rr/eval/annotate.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/knowledge/artifacts.hpp>
#include <rr/testkit/result.hpp>

#include "fixtures.hpp"

namespace rr::eval {
namespace {

using knowledge::SentenceId;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using namespace std::chrono_literals;
using testkit::unwrap;

// Nine sentences, four seconds each.
knowledge::Transcript lecture() {
    return fixtures::transcript_of({{"Welcome back to the shed.", 0s, 4s},
                                    {"Today we weigh the radium salts.", 4s, 8s},
                                    {"The first sample failed at four tons, so we tried again.", 8s, 12s},
                                    {"We call that the proof load.", 12s, 16s},
                                    {"Marie Curie measured 7 samples in 1902.", 16s, 20s},
                                    {"That took months.", 20s, 24s},
                                    {"The samples glowed faintly in the dark.", 24s, 28s},
                                    {"Polonium came first.", 28s, 32s},
                                    {"Thank you.", 32s, 36s}});
}

// `count` sentences of 13 bytes, four seconds apart: lines of 24 bytes with
// their newline ("S1 [0:00] Radium glows.\n"), 25 from S10 on.
knowledge::Transcript glowing(std::size_t count = 10) {
    std::vector<fixtures::Said> said;
    said.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto start = std::chrono::seconds{4 * static_cast<std::int64_t>(index)};
        said.push_back({"Radium glows.", start, start + 4s});
    }
    return fixtures::transcript_of(said);
}

std::vector<SentenceId> ids(std::initializer_list<std::uint32_t> numbers) {
    std::vector<SentenceId> all;
    all.reserve(numbers.size());
    for (const std::uint32_t number : numbers) {
        all.emplace_back(number);
    }
    return all;
}

// --- Windows ----------------------------------------------------------------

TEST(Windows, ShowTheirSentencesALineEach) {
    EXPECT_EQ(sentence_lines(glowing(), 0, 2), "S1 [0:00] Radium glows.\nS2 [0:04] Radium glows.");
    EXPECT_EQ(sentence_lines(glowing(), 9, 12), "S10 [0:36] Radium glows.") << "to the transcript's end";
    EXPECT_EQ(sentence_lines(glowing(), 3, 3), "");
}

TEST(Windows, HoldAsManyWholeSentencesAsTheirLinesFitOneAtLeast) {
    const knowledge::Transcript transcript = glowing();
    EXPECT_EQ(window_from(transcript, 0, 72).end, 3U) << "three lines of 24 bytes";
    EXPECT_EQ(window_from(transcript, 0, 71).end, 2U);
    EXPECT_EQ(window_from(transcript, 0, 1).end, 1U) << "one at least";
    EXPECT_EQ(window_from(transcript, 8, 10'000).end, 10U) << "to the end";
    EXPECT_EQ(window_from(transcript, 4, 72),
              (SentenceWindow{.begin = 4, .end = 7, .context_begin = 1, .context_end = 10}));
}

TEST(Windows, TheirContextIsAFewSentencesOnEitherSideWithinItsBytes) {
    const knowledge::Transcript transcript = glowing();
    EXPECT_EQ(window_of(transcript, 4, 6),
              (SentenceWindow{.begin = 4, .end = 6, .context_begin = 1, .context_end = 9}))
        << "three on either side";
    EXPECT_EQ(window_of(transcript, 0, 2),
              (SentenceWindow{.begin = 0, .end = 2, .context_begin = 0, .context_end = 5}))
        << "none before the first";
    EXPECT_EQ(window_of(transcript, 8, 10),
              (SentenceWindow{.begin = 8, .end = 10, .context_begin = 5, .context_end = 10}))
        << "none after the last";
    EXPECT_EQ(window_of(transcript, 4, 6, 0),
              (SentenceWindow{.begin = 4, .end = 6, .context_begin = 4, .context_end = 6}));

    // Before the window, two sentences of 600 bytes: one fits 1,000 bytes.
    std::vector<fixtures::Said> said;
    for (std::size_t index = 0; index < 9; ++index) {
        const auto start = std::chrono::seconds{4 * static_cast<std::int64_t>(index)};
        said.push_back(
            {index == 2 || index == 3 ? std::string(600, 'x') : "Radium glows.", start, start + 4s});
    }
    EXPECT_EQ(window_of(fixtures::transcript_of(said), 4, 6),
              (SentenceWindow{.begin = 4, .end = 6, .context_begin = 3, .context_end = 9}));
}

TEST(Windows, AreSizedToTheirEngines) {
    const auto sizes_of = [](std::vector<agents::Engine> engines) {
        const fixtures::Rig rig{{}, std::move(engines)};
        return window_sizes(*rig.army);
    };
    // The commander: (8192 - 1200 - 504 of thinking) / 3 tokens of 3 bytes.
    // The infantry: (4096 - 1200 - 1024 of answer) / 2 tokens of 3 bytes.
    EXPECT_EQ(sizes_of(fixtures::engines()), (WindowSizes{.annotate = 6'486, .more = 2'808}));
    EXPECT_EQ(sizes_of(fixtures::engines(65'536, 65'536)), (WindowSizes{.annotate = 12'000, .more = 3'000}))
        << "at most";
    EXPECT_EQ(sizes_of(fixtures::engines(2'048, 2'048)), (WindowSizes{.annotate = 1'500, .more = 1'500}))
        << "at least";

    std::vector<agents::Engine> alone{fixtures::engines().front()};
    alone.front().roles = {"commander", "infantry"};
    EXPECT_EQ(sizes_of(alone), (WindowSizes{.annotate = 6'486, .more = 3'000}))
        << "one engine, both readings";
}

// --- Drafts -----------------------------------------------------------------

json::Json item(std::vector<std::string> sentences, std::string quote, std::string_view importance = "core") {
    return json::Json{{"sentences", std::move(sentences)},
                      {"quote", std::move(quote)},
                      {"claim", "A claim."},
                      {"importance", std::string{importance}}};
}

json::Json quoted(std::vector<std::string> sentences, std::string quote) {
    return json::Json{{"sentences", std::move(sentences)}, {"quote", std::move(quote)}};
}

json::Json termed(std::string term, std::string meaning = "") {
    return json::Json{{"term", std::move(term)}, {"meaning", std::move(meaning)}};
}

json::Json answer(json::Json facts, json::Json must_quotes = json::Json::array(),
                  json::Json terms = json::Json::array()) {
    return json::Json{
        {"facts", std::move(facts)}, {"must_quotes", std::move(must_quotes)}, {"terms", std::move(terms)}};
}

TEST(Drafts, AreReadWithTheirSentencesInOrderAndOnce) {
    const Draft draft =
        draft_from_json(answer(json::Json::array({item({"S5", "S4", "S5"}, "proof", "detail")}),
                               json::Json::array({quoted({"S4"}, "the proof load")}),
                               json::Json::array({termed("proof load", "the most it carries")})));
    ASSERT_EQ(draft.facts.size(), 1U);
    EXPECT_EQ(draft.facts[0].sentences, ids({4, 5}));
    EXPECT_EQ(draft.facts[0].quote, "proof");
    EXPECT_EQ(draft.facts[0].claim, "A claim.");
    EXPECT_EQ(draft.facts[0].importance, Importance::detail);
    ASSERT_EQ(draft.must_quotes.size(), 1U);
    EXPECT_EQ(draft.must_quotes[0].sentences, ids({4}));
    EXPECT_EQ(draft.must_quotes[0].quote, "the proof load");
    EXPECT_THAT(draft.terms, ElementsAre(Term{.term = "proof load", .meaning = "the most it carries"}));
}

TEST(Drafts, ThatSayWhatTheWindowSaysPass) {
    const knowledge::Transcript transcript = lecture();
    EXPECT_THAT(draft_problems(answer(json::Json::array({item({"S4"}, "the proof load"),
                                                         item({"S5", "S6"},
                                                              "MARIE CURIE measured 7 samples in 1902 that "
                                                              "took months")}),
                                      json::Json::array({quoted({"S4"}, "We call that the proof load.")}),
                                      json::Json::array({termed("Proof Load")})),
                               transcript, window_of(transcript, 3, 6)),
                IsEmpty())
        << "across sentences, case and punctuation aside";
}

TEST(Drafts, ThatDoNotAreSentBackWithWhatToDoInstead) {
    const knowledge::Transcript transcript = lecture();
    const SentenceWindow window = window_of(transcript, 3, 6);  // S4–S6, read with S1–S3 and S7–S9
    EXPECT_THAT(
        draft_problems(
            answer(json::Json::array({item({"S2"}, "the radium salts"), item({"S4", "S7"}, "proof load"),
                                      item({"S5"}, "Pierre Curie measured 7 samples"),
                                      item({"S4", "S5"}, "the proof load Pierre")}),
                   json::Json::array({quoted({"S6"}, "That took years.")}),
                   json::Json::array({termed("proof load"), termed("polonium")})),
            transcript, window),
        ElementsAre(
            "fact 1: cite only the sentences to annotate, S4\u2013S6, not S2",
            "fact 2: cite only the sentences to annotate, S4\u2013S6, not S7",
            "fact 3: \"Pierre Curie measured 7 samples\" is not what S5 says, word for word: copy the quote "
            "from it",
            "fact 4: \"the proof load Pierre\" is not what S4, S5 say, word for word: copy the quote from "
            "them",
            "must-quote 1: \"That took years.\" is not what S6 says, word for word: copy the quote from it",
            "term \"polonium\" is not said in S4\u2013S6: list only the video's own words"));

    EXPECT_THAT(draft_problems(answer(json::Json::array({item({"S5"}, "Marie Curie")})), transcript,
                               window_of(transcript, 3, 4)),
                ElementsAre("fact 1: cite only the sentences to annotate, S4, not S5"));
}

TEST(Drafts, AreSentBackAFewProblemsAtATime) {
    const knowledge::Transcript transcript = lecture();
    json::Json facts = json::Json::array();
    for (int index = 0; index < 10; ++index) {
        facts.push_back(item({"S1"}, "Welcome back"));
    }
    const std::vector<std::string> problems =
        draft_problems(answer(facts), transcript, window_of(transcript, 3, 6));
    ASSERT_EQ(problems.size(), 9U);
    EXPECT_EQ(problems[7], "fact 8: cite only the sentences to annotate, S4\u2013S6, not S1");
    EXPECT_EQ(problems[8], "and 2 more like them");
}

// --- Joining ----------------------------------------------------------------

DraftFact fact(std::initializer_list<std::uint32_t> sentences, std::string quote,
               Importance importance = Importance::core, std::string claim = "A claim.") {
    return DraftFact{.sentences = ids(sentences),
                     .quote = std::move(quote),
                     .claim = std::move(claim),
                     .importance = importance};
}

TEST(Joining, KeepsAFactEveryDraftFoundOnceAndCountsThem) {
    const Draft draft{.facts = {fact({4}, "the proof load", Importance::core, "It is the proof load.")}};
    const Silver silver = join_drafts({draft, draft, draft}, lecture());
    EXPECT_THAT(silver.facts, ElementsAre(SilverFact{.sentences = ids({4}),
                                                     .start = 12s,
                                                     .end = 16s,
                                                     .quote = "the proof load",
                                                     .claim = "It is the proof load.",
                                                     .importance = Importance::core,
                                                     .found = 3}));
}

TEST(Joining, TakesOneQuoteHoldingAnotherInASentenceInCommonForOneFact) {
    const Silver silver =
        join_drafts({Draft{.facts = {fact({3}, "four tons", Importance::core, "First.")}},
                     Draft{.facts = {fact({3, 4}, "failed at four tons, so we tried again. We call that",
                                          Importance::core, "Second.")}},
                     Draft{.facts = {fact({4}, "the proof load", Importance::core, "Third.")}}},
                    lecture());
    ASSERT_EQ(silver.facts.size(), 2U);
    EXPECT_EQ(silver.facts[0], (SilverFact{.sentences = ids({3, 4}),
                                           .start = 8s,
                                           .end = 16s,
                                           .quote = "four tons",
                                           .claim = "First.",
                                           .importance = Importance::core,
                                           .found = 2}))
        << "the first draft's words, on every sentence that says it";
    EXPECT_EQ(silver.facts[1].quote, "the proof load") << "one sentence in common, no quote in common";
    EXPECT_EQ(silver.facts[1].found, 1);
}

TEST(Joining, KeepsTwoThingsOneSentenceSaysAndAPhraseSaidTwice) {
    const Silver silver =
        join_drafts({Draft{.facts = {fact({3}, "The first sample failed"), fact({3}, "so we tried again"),
                                     fact({5}, "samples"), fact({7}, "samples")}}},
                    lecture());
    EXPECT_EQ(silver.facts.size(), 4U);
}

TEST(Joining, CountsADraftOnceHoweverOftenItListsAFact) {
    const Silver silver = join_drafts(
        {Draft{.facts = {fact({4}, "the proof load"), fact({4}, "We call that the proof load")}}}, lecture());
    ASSERT_EQ(silver.facts.size(), 1U);
    EXPECT_EQ(silver.facts[0].found, 1);
}

TEST(Joining, CallsAFactCoreUnlessMostDraftsCallItDetail) {
    const auto importance = [](std::initializer_list<Importance> votes) {
        std::vector<Draft> drafts;
        for (const Importance vote : votes) {
            drafts.push_back(Draft{.facts = {fact({4}, "the proof load", vote)}});
        }
        const Silver silver = join_drafts(drafts, lecture());
        EXPECT_EQ(silver.facts.size(), 1U);
        return silver.facts.empty() ? Importance::core : silver.facts[0].importance;
    };
    EXPECT_EQ(importance({Importance::core, Importance::detail, Importance::detail}), Importance::detail);
    EXPECT_EQ(importance({Importance::core, Importance::core, Importance::detail}), Importance::core);
    EXPECT_EQ(importance({Importance::detail, Importance::core}), Importance::core)
        << "a tie: an account that keeps too much is corrected faster";
    EXPECT_EQ(importance({Importance::detail}), Importance::detail);

    // A draft that calls a fact core twice still votes once.
    const Silver silver = join_drafts({Draft{.facts = {fact({4}, "the proof load"), fact({4}, "proof load")}},
                                       Draft{.facts = {fact({4}, "the proof load", Importance::detail)}},
                                       Draft{.facts = {fact({4}, "the proof load", Importance::detail)}}},
                                      lecture());
    ASSERT_EQ(silver.facts.size(), 1U);
    EXPECT_EQ(silver.facts[0].importance, Importance::detail);
}

TEST(Joining, PutsFactsInTheOrderSaid) {
    const Silver silver = join_drafts(
        {Draft{.facts = {fact({7}, "glowed"), fact({2}, "radium salts"),
                         fact({2, 3}, "the radium salts. The first sample"), fact({5}, "Curie")}}},
        lecture());
    std::vector<std::string> quotes;
    quotes.reserve(silver.facts.size());
    for (const SilverFact& kept : silver.facts) {
        quotes.push_back(kept.quote);
    }
    EXPECT_THAT(quotes, ElementsAre("radium salts", "Curie", "glowed")) << "the second joined the first";
    EXPECT_EQ(silver.facts[0].sentences, ids({2, 3}));
}

TEST(Joining, JoinsMustQuotesAndTermsToo) {
    const Silver silver = join_drafts(
        {Draft{.must_quotes = {DraftQuote{.sentences = ids({4}), .quote = "the proof load"}},
               .terms = {Term{.term = "proof load"}, Term{.term = "radium", .meaning = "an element"}}},
         Draft{.must_quotes = {DraftQuote{.sentences = ids({4}), .quote = "We call that the proof load"},
                               DraftQuote{.sentences = ids({1}), .quote = "Welcome back"}},
               .terms = {Term{.term = "Proof Load", .meaning = "what it carries"},
                         Term{.term = "RADIUM", .meaning = "a metal"}}}},
        lecture());
    EXPECT_THAT(
        silver.must_quotes,
        ElementsAre(SilverQuote{.sentences = ids({1}), .start = 0s, .end = 4s, .quote = "Welcome back"},
                    SilverQuote{.sentences = ids({4}), .start = 12s, .end = 16s, .quote = "the proof load"}));
    EXPECT_THAT(silver.terms, ElementsAre(Term{.term = "proof load", .meaning = "what it carries"},
                                          Term{.term = "radium", .meaning = "an element"}))
        << "one term, whatever its case; a meaning fills an empty one, and never replaces one";
}

TEST(Joining, AddsWhatTheOtherFamilyFoundThatTheDraftsMissed) {
    const knowledge::Transcript transcript = lecture();
    Silver silver = join_drafts(
        {Draft{.facts = {fact({5}, "Marie Curie measured 7 samples")}, .terms = {Term{.term = "radium"}}}},
        transcript);
    add_missed(silver,
               Draft{.facts = {fact({5}, "measured 7 samples"),
                               fact({3}, "The first sample failed", Importance::detail)},
                     .must_quotes = {DraftQuote{.sentences = ids({4}), .quote = "the proof load"}},
                     .terms = {Term{.term = "Radium", .meaning = "an element"}, Term{.term = "polonium"}}},
               transcript);
    ASSERT_EQ(silver.facts.size(), 2U) << "what the drafts found is not added again";
    EXPECT_EQ(silver.facts[0], (SilverFact{.sentences = ids({3}),
                                           .start = 8s,
                                           .end = 12s,
                                           .quote = "The first sample failed",
                                           .claim = "A claim.",
                                           .importance = Importance::detail,
                                           .found = 0}))
        << "found by no draft, and in the order said";
    EXPECT_EQ(silver.facts[1].found, 1);
    EXPECT_THAT(
        silver.must_quotes,
        ElementsAre(SilverQuote{.sentences = ids({4}), .start = 12s, .end = 16s, .quote = "the proof load"}));
    EXPECT_THAT(silver.terms,
                ElementsAre(Term{.term = "radium", .meaning = "an element"}, Term{.term = "polonium"}));
}

// --- Kept -------------------------------------------------------------------

Silver drafted() {
    return Silver{
        .facts = {SilverFact{.sentences = ids({3, 4}),
                             .start = 8s,
                             .end = 16s,
                             .quote = "four tons",
                             .claim = "The first sample failed at four tons.",
                             .importance = Importance::core,
                             .found = 3},
                  SilverFact{.sentences = ids({6}),
                             .start = 20s,
                             .end = 24s,
                             .quote = "That took months.",
                             .claim = "Measuring took months.",
                             .importance = Importance::detail,
                             .found = 0}},
        .must_quotes = {SilverQuote{.sentences = ids({4}), .start = 12s, .end = 16s, .quote = "proof load"}},
        .terms = {Term{.term = "proof load", .meaning = "the most it carries"}, Term{.term = "shed"}},
        .drafted_by = "annotate@1 \u00D73 on qwen; annotate_more@1 on gemma"};
}

TEST(Silver, IsKeptAsItsArtifactAndReadBack) {
    const json::Json document = to_json(drafted());
    EXPECT_OK(knowledge::artifact_schemas().validate("artifacts/silver", document));
    EXPECT_EQ(document.at("facts").at(0).at("sentences"), (json::Json::array({"S3", "S4"})));
    EXPECT_EQ(document.at("facts").at(0).at("start_ms"), 8000);
    EXPECT_EQ(unwrap(silver_from_json(document)), drafted());

    json::Json broken = document;
    broken["facts"][0].erase("found");
    const auto unfound = silver_from_json(broken);
    EXPECT_ERRC(unfound, Errc::invalid_argument);
    EXPECT_THAT(unfound.error().message(), HasSubstr("not a silver annotation"));
    broken = document;
    broken["drafted_by"] = "";
    EXPECT_ERRC(silver_from_json(broken), Errc::invalid_argument) << "silver says who drafted it";
}

TEST(Silver, BecomesTheFirstGoldFileUnreviewed) {
    const Gold gold = silver_to_gold(drafted(), "radium-talk", "4f2a9c");
    EXPECT_EQ(gold, (Gold{.video = "radium-talk",
                          .snapshot = "4f2a9c",
                          .status = GoldStatus::silver,
                          .drafted_by = drafted().drafted_by,
                          .facts = {Fact{.id = "F1",
                                         .start = 8s,
                                         .end = 16s,
                                         .importance = Importance::core,
                                         .quote = "four tons",
                                         .claim = "The first sample failed at four tons.",
                                         .found = 3},
                                    Fact{.id = "F2",
                                         .start = 20s,
                                         .end = 24s,
                                         .importance = Importance::detail,
                                         .quote = "That took months.",
                                         .claim = "Measuring took months.",
                                         .found = 0}},
                          .must_quotes = {MustQuote{.start = 12s, .end = 16s, .quote = "proof load"}},
                          .terms = drafted().terms}))
        << "times and quotes, never sentence numbers: gold outlives a transcript's segmentation";
}

}  // namespace
}  // namespace rr::eval

#include <rr/knowledge/transcript.hpp>

#include <string>

#include <gtest/gtest.h>

#include <rr/core/error.hpp>
#include <rr/testkit/result.hpp>

namespace rr::knowledge {
namespace {

using testkit::unwrap;

TimeSpan span(std::int64_t start, std::int64_t end) {
    return unwrap(TimeSpan::make(Millis{start}, Millis{end}));
}

Transcript two_sentences() {
    return Transcript{
        .sentences = {
            Sentence{.id = SentenceId{1}, .text = "Water boils.", .span = span(0, 1200), .speaker = "Ada"},
            Sentence{.id = SentenceId{2}, .text = "Ice melts.", .span = span(1200, 2500)},
        }};
}

TEST(Transcript, RoundTripsThroughItsArtifact) {
    const Transcript transcript = two_sentences();
    const json::Json document = to_json(transcript);
    EXPECT_EQ(document.at("sentences").at(1).at("speaker"), nullptr)
        << "an unknown speaker is null, never guessed";
    EXPECT_EQ(unwrap(transcript_from_json(document)), transcript);
}

TEST(Transcript, FindsASentenceByItsId) {
    const Transcript transcript = two_sentences();
    ASSERT_NE(transcript.find(SentenceId{2}), nullptr);
    EXPECT_EQ(transcript.find(SentenceId{2})->text, "Ice melts.");
    EXPECT_EQ(transcript.find(SentenceId{3}), nullptr);
    const Transcript out_of_place{.sentences = {two_sentences().sentences[1]}};
    ASSERT_NE(out_of_place.find(SentenceId{2}), nullptr) << "a transcript built in code is searched";
    EXPECT_EQ(out_of_place.find(SentenceId{1}), nullptr);
}

void expect_refused(const json::Json& document, std::string_view because) {
    const Result<Transcript> read = transcript_from_json(document);
    ASSERT_FALSE(read.has_value()) << because;
    EXPECT_EQ(read.error().code(), Errc::invalid_argument);
    EXPECT_NE(read.error().message().find(because), std::string::npos) << read.error().message();
}

TEST(Transcript, SentencesRunFromS1InOrder) {
    json::Json document = to_json(two_sentences());
    document["sentences"][1]["id"] = "S3";
    expect_refused(document, "S3 is out of place");
    document["sentences"][0]["id"] = "S2";
    expect_refused(document, "S2 is out of place");
}

TEST(Transcript, ASentenceEndsAfterItStarts) {
    json::Json document = to_json(two_sentences());
    document["sentences"][1]["end_ms"] = 1000;
    expect_refused(document, "S2 ends before it starts");
}

TEST(Transcript, SentencesAreInTheOrderSaid) {
    json::Json document = to_json(two_sentences());
    document["sentences"][0]["start_ms"] = 1000;
    document["sentences"][1]["start_ms"] = 100;
    expect_refused(document, "S2 starts before S1");
}

TEST(Transcript, TheSchemaIsStrict) {
    json::Json document = to_json(two_sentences());
    document["sentences"][0]["confidence"] = 0.9;
    expect_refused(document, "schema violation");
    json::Json missing = to_json(two_sentences());
    missing["sentences"][0].erase("speaker");
    expect_refused(missing, "schema violation");
    json::Json negative = to_json(two_sentences());
    negative["sentences"][0]["start_ms"] = -1;
    expect_refused(negative, "schema violation");
}

}  // namespace
}  // namespace rr::knowledge

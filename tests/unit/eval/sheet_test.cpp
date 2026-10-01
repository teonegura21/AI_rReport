// The review sheet: each fact beside the moment that says it, checked
// against the snapshot's captions without a model.
#include <rr/eval/sheet.hpp>

#include <chrono>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>

#include "fixtures.hpp"

namespace rr::eval {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;
using namespace std::chrono_literals;
using testkit::unwrap;

// Three sentences: 0–4 s, 4–9 s and 9–15 s.
std::vector<fixtures::Said> lecture() {
    return {{"Welcome back to the shed.", 0s, 4s},
            {"The first sample failed at four tons, so we tried again.", 4s, 9s},
            {"We call that the proof load.", 9s, 15s}};
}

Gold reviewed() {
    return Gold{
        .video = "radium-talk",
        .snapshot = "4f2a9c51d0e7b3a8",
        .status = GoldStatus::gold,
        .reviewer = "Ada",
        .drafted_by = "annotate@1 \u00D73 on qwen; annotate_more@1 on gemma",
        .facts = {Fact{.id = "F1",
                       .start = 4s,
                       .end = 9s,
                       .importance = Importance::core,
                       .quote = "the first sample failed",
                       .claim = "The first sample | failed\nunder load.",
                       .found = 1},
                  Fact{.id = "F2",
                       .start = 9s,
                       .end = 15s,
                       .importance = Importance::detail,
                       .quote = "We call that the proof load.",
                       .claim = "It is the proof load.",
                       .found = 3}},
        .must_quotes = {MustQuote{.start = 9s, .end = 15s, .quote = "the proof load"}},
        .terms = {Term{.term = "proof load", .meaning = "the most it carries"}, Term{.term = "shed"}}};
}

TEST(Sheet, AnchorsAreCheckedAgainstTheCaptionsCutAsTheyAre) {
    const pipeline::Source source =
        fixtures::source_of("WEBVTT\n\n00:00.000 --> 00:04.000\nRadium glows. Pierre heated it.\n");
    const knowledge::Transcript transcript = unwrap(caption_transcript(source));
    ASSERT_EQ(transcript.sentences.size(), 2U) << "no model punctuates them";
    EXPECT_EQ(transcript.sentences[1].text, "Pierre heated it.");
    EXPECT_ERRC(caption_transcript(fixtures::source_of("WEBVTT\n\nnot a cue\n")), Errc::invalid_argument);
}

TEST(Sheet, LinksEachMomentOfTheVideo) {
    EXPECT_EQ(at_time("https://www.youtube.com/watch?v=abc", 184'750ms),
              "https://www.youtube.com/watch?v=abc&t=184s")
        << "to the second, as players take it";
    EXPECT_EQ(at_time("https://youtu.be/abc", 0ms), "https://youtu.be/abc?t=0s");
}

TEST(Sheet, PutsEachFactBesideItsMomentAndSaysWhatToCorrect) {
    const std::string sheet =
        review_sheet(reviewed(), fixtures::video(), fixtures::transcript_of(lecture()), reviewed().snapshot);
    EXPECT_THAT(
        sheet,
        HasSubstr("# Review: On radium\n\nhttps://www.youtube.com/watch?v=abc123XYZ_0 \u00B7 snapshot "
                  "`4f2a9c51d0e7` \u00B7 gold, drafted by annotate@1 \u00D73 on qwen; annotate_more@1 on "
                  "gemma\n"));
    EXPECT_THAT(sheet,
                HasSubstr("Correct `gold/radium-talk.toml`, in the eval's directory, beside this sheet"));
    EXPECT_THAT(sheet, HasSubstr("## Facts: 2 (1 core, 1 detail)\n\n"
                                 "| Fact | When | Found | Importance | Claim | Quote | Said there |\n"));
    EXPECT_THAT(sheet,
                HasSubstr("| F1 ? | [0:04\u20130:09](https://www.youtube.com/watch?v=abc123XYZ_0&t=4s) | 1 | "
                          "core | The first sample \\| failed under load. | \"the first sample failed\" | "
                          "\u2713 |\n"))
        << "a fact one draft found is the first to doubt; a cell stays one cell";
    EXPECT_THAT(sheet,
                HasSubstr("| F2 | [0:09\u20130:15](https://www.youtube.com/watch?v=abc123XYZ_0&t=9s) | 3 | "
                          "detail | It is the proof load. | \"We call that the proof load.\" | \u2713 |\n"));
    EXPECT_THAT(sheet,
                HasSubstr("## Must-quotes: 1\n\n| # | When | Quote | Said there |\n|---|---|---|---|\n"
                          "| 1 | [0:09\u20130:15](https://www.youtube.com/watch?v=abc123XYZ_0&t=9s) | \"the "
                          "proof load\" | \u2713 |\n"));
    EXPECT_THAT(
        sheet,
        HasSubstr("## Terms: 2\n\n| Term | Meaning |\n|---|---|\n| proof load | the most it carries |\n"
                  "| shed | \u2014 |\n"));
    EXPECT_THAT(sheet, HasSubstr("## To correct\n\nNothing the sheet can check."));

    Gold wrong = reviewed();
    wrong.facts[1].quote = "We call it the proof load.";
    wrong.reviewer.clear();
    wrong.drafted_by.clear();
    const std::string corrected =
        review_sheet(wrong, fixtures::video(), fixtures::transcript_of(lecture()), "77e0b1d2c3a4f5e6");
    EXPECT_THAT(corrected, HasSubstr("\"We call it the proof load.\" | \u2717 not said there |"));
    EXPECT_THAT(
        corrected,
        HasSubstr("## To correct\n\n- it was drafted on the snapshot 4f2a9c51d0e7b3a8, and the snapshot "
                  "is now 77e0b1d2c3a4f5e6: check every fact against the new captions, then set snapshot = "
                  "\"77e0b1d2c3a4f5e6\"\n- F2: \"We call it the proof load.\" is not said between 0:09 "
                  "and 0:15: correct its quote, or its times\n- it is gold, and names no reviewer"));
    EXPECT_THAT(corrected, Not(HasSubstr("Nothing the sheet can check")));
    EXPECT_THAT(corrected, HasSubstr("\u00B7 gold\n")) << "nobody drafted it";
}

}  // namespace
}  // namespace rr::eval

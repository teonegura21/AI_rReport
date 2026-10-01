// Gold: written for people, read back strictly, and anchored to the timeline.
#include <rr/eval/gold.hpp>

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>

#include "fixtures.hpp"

namespace rr::eval {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using namespace std::chrono_literals;
using testkit::unwrap;

constexpr std::string_view kFile = "eval/gold/radium-talk.toml";

// Three sentences: 0–4 s, 4–9 s and 9–15 s.
knowledge::Transcript lecture() {
    return fixtures::transcript_of({{"Welcome back to the shed.", 0s, 4s},
                                    {"The first sample failed at four tons, so we tried again.", 4s, 9s},
                                    {"We call that the proof load.", 9s, 15s}});
}

Gold reviewed() {
    return Gold{
        .video = "radium-talk",
        .snapshot = "4f2a9c",
        .status = GoldStatus::gold,
        .reviewer = "Ada",
        .drafted_by = "annotate@1 \u00D73 on qwen; annotate_more@1 on gemma",
        .facts = {Fact{.id = "F1",
                       .start = 4s,
                       .end = 9s,
                       .importance = Importance::core,
                       .quote = "the first sample failed at four tons",
                       .claim = "The first sample failed under four tons.",
                       .found = 3},
                  Fact{.id = "F7",
                       .start = 9s,
                       .end = 15s,
                       .importance = Importance::detail,
                       .quote = "We call that the proof load.",
                       .claim = "That load is called the proof load.",
                       .found = 0}},
        .must_quotes = {MustQuote{.start = 9s, .end = 15s, .quote = "the proof load"}},
        .terms = {Term{.term = "proof load", .meaning = "the most a sample carries"}, Term{.term = "shed"}}};
}

TEST(Gold, IsWrittenForPeopleAndReadBackAsItWas) {
    Gold gold = reviewed();
    gold.facts[0].start = 3'723'250ms;  // 1:02:03.25
    gold.facts[0].end = 3'725'000ms;
    gold.facts[1].claim =
        "Quotes \"inside\", a back\\slash,\ta tab,\nnew lines\r\nand a bell \x07: caf\u00E9.";
    const std::string written = to_toml(gold);
    EXPECT_THAT(written, HasSubstr("# The gold annotation of radium-talk")) << "it explains itself";
    EXPECT_THAT(written, HasSubstr("\n[[fact]]\nid = \"F1\"\nstart = \"1:02:03.25\"\nend = \"1:02:05\"\n"))
        << "as a player shows the times";
    EXPECT_THAT(written, HasSubstr(R"(\"inside\", a back\\slash,\ta tab,\nnew lines\r\nand a bell \u0007: )"
                                   "caf\u00E9."));
    EXPECT_EQ(unwrap(parse_gold(written, kFile)), gold);

    const Gold silver{.video = "radium-talk", .snapshot = "4f2a9c"};
    EXPECT_EQ(unwrap(parse_gold(to_toml(silver), kFile)), silver) << "nothing found yet, nobody named";
}

TEST(Gold, ATimeIsWrittenAsPlayersShowIt) {
    EXPECT_EQ(clock_time(0ms), "0:00");
    EXPECT_EQ(clock_time(184s), "3:04");
    EXPECT_EQ(clock_time(184'250ms), "3:04.25");
    EXPECT_EQ(clock_time(184'005ms), "3:04.005");
    EXPECT_EQ(clock_time(3'599s), "59:59");
    EXPECT_EQ(clock_time(3'600s), "1:00:00") << "from the first hour on, as players show it";
    EXPECT_EQ(clock_time(3'723'500ms), "1:02:03.5");
    EXPECT_EQ(clock_time(36'000s), "10:00:00");
    EXPECT_EQ(clock_time(-5s), "0:00") << "never before the video";
}

// A gold file with one mistake, and what the message must say of it.
struct Mistake {
    std::string_view why;
    std::string_view find;     // in to_toml(reviewed()), once
    std::string_view replace;  // what it becomes
    std::string_view message;
};

TEST(Gold, EveryMistakeIsRefusedNamingItAndWhereItIs) {
    const std::vector<Mistake> mistakes{
        Mistake{"an unknown key", "reviewer = \"Ada\"", "reviewr = \"Ada\"",
                "eval/gold/radium-talk.toml: unknown key 'reviewr'"},
        Mistake{"a status out of its choices", "\nstatus = \"gold\"", "\nstatus = \"approved\"",
                "'status' must be one of: silver, gold"},
        Mistake{"a video that is no video id", "video = \"radium-talk\"", "video = \"Radium Talk\"",
                "video 'Radium Talk' is not a video id"},
        Mistake{"no snapshot", "snapshot = \"4f2a9c\"\n", "", "'snapshot' must be a text, and it is missing"},
        Mistake{"a fact id in lower case", "id = \"F7\"", "id = \"f7\"",
                "fact 2: id 'f7' must be F and a number: F1, F2, \u2026"},
        Mistake{"a fact id with a leading zero", "id = \"F7\"", "id = \"F07\"", "fact 2: id 'F07' must be F"},
        Mistake{"a fact id with letters after its number", "id = \"F7\"", "id = \"F7b\"",
                "fact 2: id 'F7b' must be F"},
        Mistake{"a fact id too long", "id = \"F7\"", "id = \"F1234567890\"",
                "fact 2: id 'F1234567890' must be F"},
        Mistake{"two facts with one id", "id = \"F7\"", "id = \"F1\"", "fact 2: a second fact F1"},
        Mistake{"a fact without its importance", "importance = \"detail\"\n", "",
                "fact 2: 'importance' must be a text, and it is missing or empty"},
        Mistake{"an importance out of its choices", "importance = \"detail\"", "importance = \"minor\"",
                "fact 2: 'importance' must be one of: core, detail"},
        Mistake{"a time that is no time", "start = \"0:09\"\nend = \"0:15\"\nimportance",
                "start = \"nine seconds\"\nend = \"0:15\"\nimportance",
                R"(fact 2: 'start' must be a time such as "3:04" or "1:02:03.250", not "nine seconds")"},
        Mistake{"found out of its range", "found = 3", "found = 100",
                "fact 1: 'found' must be a whole number from 0 to 99"},
        Mistake{"found in quotes", "found = 3", "found = \"3\"", "fact 1: 'found' must be a whole number"},
        Mistake{"an unknown key in a fact", "found = 3", "found = 3\nspeaker = \"Ada\"",
                "fact 1: unknown key 'speaker'"},
        Mistake{"a must-quote without its quote", "quote = \"the proof load\"\n", "",
                "must_quote 1: 'quote' must be a text"},
        Mistake{"a term without its term", "term = \"shed\"\n", "", "term 2: 'term' must be a text"},
        Mistake{"broken TOML", "reviewer = \"Ada\"", "reviewer = \"Ada",
                "eval/gold/radium-talk.toml, line "}};
    for (const Mistake& mistake : mistakes) {
        std::string text = to_toml(reviewed());
        const std::size_t at = text.find(mistake.find);
        ASSERT_NE(at, std::string::npos) << mistake.why;
        ASSERT_EQ(text.find(mistake.find, at + 1), std::string::npos)
            << mistake.why << ": the edit must be unique";
        text.replace(at, mistake.find.size(), mistake.replace);

        const Result<Gold> gold = parse_gold(text, kFile);
        EXPECT_ERRC(gold, Errc::invalid_argument) << mistake.why;
        if (!gold.has_value()) {
            EXPECT_THAT(gold.error().message(), HasSubstr(mistake.message)) << mistake.why;
        }
    }
}

TEST(Gold, FactsAreTables) {
    const Result<Gold> gold = parse_gold(
        "video = \"radium-talk\"\nsnapshot = \"4f2a9c\"\nstatus = \"silver\"\nfact = \"F1\"\n", kFile);
    EXPECT_ERRC(gold, Errc::invalid_argument);
    if (!gold.has_value()) {
        EXPECT_THAT(gold.error().message(), HasSubstr("'fact' must be [[fact]] tables"));
    }
}

TEST(Gold, AQuoteIsSaidThereWhenItsWordsAreSaidNearItsStretch) {
    const knowledge::Transcript transcript = lecture();
    EXPECT_TRUE(said_there("the first sample failed at four tons", 4s, 9s, transcript));
    EXPECT_TRUE(said_there("THE FIRST SAMPLE FAILED, at four tons", 4s, 9s, transcript))
        << "case and punctuation aside";
    EXPECT_TRUE(said_there("so we tried again. We call that", 4s, 15s, transcript)) << "across sentences";
    EXPECT_FALSE(said_there("the first sample failed at five tons", 4s, 9s, transcript))
        << "not what was said";

    EXPECT_TRUE(said_there("Welcome back", 6s, 7s, transcript)) << "two seconds early: within the slack";
    EXPECT_FALSE(said_there("Welcome back", 6'001ms, 7s, transcript)) << "beyond it";
    EXPECT_TRUE(said_there("the proof load", 2s, 7s, transcript)) << "two seconds late";
    EXPECT_FALSE(said_there("the proof load", 2s, 6'999ms, transcript));

    EXPECT_FALSE(said_there("the proof load", 9s, 15s, knowledge::Transcript{})) << "nothing said";
}

TEST(Gold, ProblemsSayWhatToCorrect) {
    const knowledge::Transcript transcript = lecture();
    EXPECT_THAT(gold_problems(reviewed(), transcript), IsEmpty());

    Gold gold = reviewed();
    gold.facts[0].end = 3s;                            // before it starts, at 4 s
    gold.facts[1].quote = std::string(70, 'x') + "y";  // said nowhere, and long
    gold.must_quotes[0].start = 30s;
    gold.must_quotes[0].end = 35s;
    gold.reviewer.clear();
    EXPECT_THAT(
        gold_problems(gold, transcript),
        ElementsAre("F1 ends at 0:03, before it starts at 0:04: correct its times",
                    "F7: \"" + std::string(60, 'x') +
                        "\u2026\" is not said between 0:09 and 0:15: correct its quote, or its times",
                    "must-quote 1: \"the proof load\" is not said between 0:30 and 0:35: correct its quote, "
                    "or its times",
                    "it is gold, and names no reviewer: set reviewer to who corrected it"));

    gold = reviewed();
    gold.facts[1].quote = std::string(59, 'x') + "\u00E9\u00E9";  // the cut falls inside the first é
    ASSERT_EQ(gold_problems(gold, transcript).size(), 1U);
    EXPECT_THAT(gold_problems(gold, transcript)[0], HasSubstr("\"" + std::string(59, 'x') + "\u2026\""))
        << "never inside a character";

    gold = reviewed();
    gold.status = GoldStatus::silver;
    gold.reviewer.clear();
    EXPECT_THAT(gold_problems(gold, transcript), IsEmpty()) << "silver needs no reviewer";
}

TEST(Gold, IsFiledByVideo) {
    EXPECT_EQ(gold_file("eval", "radium-talk"), std::filesystem::path{"eval/gold/radium-talk.toml"});
    EXPECT_EQ(to_string(Importance::core), "core");
    EXPECT_EQ(to_string(Importance::detail), "detail");
    EXPECT_EQ(to_string(GoldStatus::silver), "silver");
    EXPECT_EQ(to_string(GoldStatus::gold), "gold");
}

}  // namespace
}  // namespace rr::eval

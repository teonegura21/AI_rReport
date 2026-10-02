// What every stage that asks models says the same way: the video, as a
// prompt's shared part shows it, and why a mission ended without an answer.
#include <rr/pipeline/asking.hpp>

#include <string>

#include <gtest/gtest.h>

#include "idle_army.hpp"

namespace rr::pipeline {
namespace {

using testing::IdleArmy;

TEST(Asking, ADescriptionNoLongerThanItsBytesIsShownWhole) {
    EXPECT_EQ(description_excerpt(""), "");
    const std::string full(kDescriptionBytes, 'x');
    EXPECT_EQ(description_excerpt(full), full);
}

TEST(Asking, ALongerDescriptionIsCutAfterALineOrAWordAndSaysSo) {
    std::string lines;
    while (lines.size() <= kDescriptionBytes) {
        lines += "Part: how the Curies measured what they could not see.\n";  // 55 bytes
    }
    const std::string cut = description_excerpt(lines);
    EXPECT_EQ(cut, lines.substr(0, 1'485) + "\u2026") << "after the 27th line, its line end kept";

    std::string words;
    while (words.size() <= kDescriptionBytes) {
        words += "radium ";  // 7 bytes
    }
    EXPECT_EQ(description_excerpt(words), words.substr(0, 1'497) + "\u2026") << "after the 214th word";
}

TEST(Asking, ADescriptionWithoutASpaceIsCutBeforeACharacterNeverInsideOne) {
    std::string accents = "x";  // then "é" (two bytes) at odd offsets: byte 1,500 is inside one
    while (accents.size() <= kDescriptionBytes) {
        accents += "\u00E9";
    }
    EXPECT_EQ(description_excerpt(accents), accents.substr(0, 1'499) + "\u2026");
    std::string even;  // here byte 1,500 starts one
    while (even.size() <= kDescriptionBytes) {
        even += "\u00E9";
    }
    EXPECT_EQ(description_excerpt(even), even.substr(0, 1'500) + "\u2026");
}

TEST(Asking, AVideoIsShownByItsTitleItsChannelAndItsDescriptionsStart) {
    const media::Video video{.id = "abc123XYZ_0",
                             .title = "On radium",
                             .description = std::string(2'000, 'x'),
                             .channel = "Science Hour"};
    const json::Json shared = video_shared(video);
    EXPECT_EQ(shared, (json::Json{{"title", "On radium"},
                                  {"channel", "Science Hour"},
                                  {"description", description_excerpt(video.description)}}));
}

TEST(Asking, TheEngineServingARoleIsFound) {
    const IdleArmy idle;
    const agents::Engine* commander = engine_serving(idle.army(), "commander");
    ASSERT_NE(commander, nullptr);
    EXPECT_EQ(commander->model, "qwen-commander");
    EXPECT_EQ(engine_serving(idle.army(), "infantry"), nullptr) << "no engine serves it";
}

TEST(Asking, AMissionWithoutAnAnswerSaysWhy) {
    const IdleArmy idle;
    EXPECT_EQ(
        why_unanswered(agents::Outcome{.verdict = agents::Verdict::refused}, idle.army(), "outline them"),
        "the models declined to outline them");
    EXPECT_EQ(why_unanswered(agents::Outcome{.verdict = agents::Verdict::invalid,
                                             .problems = {"section 2 starts at S90", "a title of 14 words"}},
                             idle.army(), "outline them"),
              "no answer passed its checks (section 2 starts at S90; a title of 14 words)");
    EXPECT_EQ(
        why_unanswered(agents::Outcome{.verdict = agents::Verdict::ceiling}, idle.army(), "outline them"),
        "the job reached its spending ceiling first")
        << "an army that names no ceiling it hit";
}

}  // namespace
}  // namespace rr::pipeline

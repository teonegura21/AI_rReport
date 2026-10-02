// Windows of a transcript: whole sentences shown a line each, a little
// context on either side, filled to a size and then given up until the
// engine that reads them holds their mission.
#include <rr/pipeline/windows.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <rr/core/error.hpp>
#include <rr/llm/chat.hpp>
#include <rr/testkit/result.hpp>

#include "idle_army.hpp"

namespace rr::pipeline {
namespace {

using testing::IdleArmy;
using testkit::unwrap;

// The transcript of `texts`, a sentence each, four seconds apart.
knowledge::Transcript transcript_of(const std::vector<std::string>& texts) {
    knowledge::Transcript transcript;
    transcript.sentences.reserve(texts.size());
    for (std::size_t index = 0; index < texts.size(); ++index) {
        const Millis start{static_cast<std::int64_t>(index) * 4'000};
        transcript.sentences.push_back(
            knowledge::Sentence{.id = knowledge::SentenceId{static_cast<std::uint32_t>(index + 1)},
                                .text = texts[index],
                                .span = unwrap(TimeSpan::make(start, start + Millis{4'000}))});
    }
    return transcript;
}

// `count` sentences of 13 bytes, four seconds apart: lines of 24 bytes with
// their newline ("S1 [0:00] Radium glows.\n"), 25 from S10 on.
knowledge::Transcript glowing(std::size_t count = 10) {
    return transcript_of(std::vector<std::string>(count, "Radium glows."));
}

// Thirty sentences of five lengths, from 13 bytes to 69.
knowledge::Transcript uneven() {
    std::vector<std::string> texts;
    texts.reserve(30);
    for (std::size_t index = 0; index < 30; ++index) {
        std::string text = "Radium glows.";
        for (std::size_t more = 0; more < index % 5; ++more) {
            text += " It glows on.";
        }
        texts.push_back(std::move(text));
    }
    return transcript_of(texts);
}

TEST(SentenceWindows, ShowTheirSentencesALineEach) {
    EXPECT_EQ(sentence_lines(glowing(), 0, 2), "S1 [0:00] Radium glows.\nS2 [0:04] Radium glows.");
    EXPECT_EQ(sentence_lines(glowing(), 9, 12), "S10 [0:36] Radium glows.") << "to the transcript's end";
    EXPECT_EQ(sentence_lines(glowing(), 3, 3), "");
}

TEST(SentenceWindows, NameTheirSentences) {
    EXPECT_EQ(sentence_range(3, 4), "S4");
    EXPECT_EQ(sentence_range(3, 6), "S4\u2013S6");
    const knowledge::Transcript transcript = glowing();
    EXPECT_EQ(sentences_have(transcript, 0, 1), "the sentence S1 (0:00 to 0:04) has");
    EXPECT_EQ(sentences_have(transcript, 0, 2), "the sentences S1\u2013S2 (0:00 to 0:08) have");
}

TEST(SentenceWindows, CountTheBytesOfTheirOwnLinesEachWithItsNewline) {
    const knowledge::Transcript transcript = glowing();
    EXPECT_EQ(lines_bytes(transcript, window_of(transcript, 0, 3)), 72U) << "the context is not counted";
    EXPECT_EQ(lines_bytes(transcript, window_of(transcript, 8, 10)), 49U) << "S10's line is a byte longer";
    EXPECT_EQ(lines_bytes(transcript, window_of(transcript, 4, 4)), 0U);
}

TEST(SentenceWindows, HoldAsManyWholeSentencesAsTheirLinesFitOneAtLeast) {
    const knowledge::Transcript transcript = glowing();
    EXPECT_EQ(window_from(transcript, 0, 72).end, 3U) << "three lines of 24 bytes";
    EXPECT_EQ(window_from(transcript, 0, 71).end, 2U);
    EXPECT_EQ(window_from(transcript, 0, 1).end, 1U) << "one at least";
    EXPECT_EQ(window_from(transcript, 8, 10'000).end, 10U) << "to the end";
    EXPECT_EQ(window_from(transcript, 4, 72),
              (SentenceWindow{.begin = 4, .end = 7, .context_begin = 1, .context_end = 10}));
}

TEST(SentenceWindows, TheirContextIsAFewSentencesOnEitherSideWithinItsBytes) {
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
    std::vector<std::string> texts(9, "Radium glows.");
    texts[2] = std::string(600, 'x');
    texts[3] = std::string(600, 'x');
    EXPECT_EQ(window_of(transcript_of(texts), 4, 6),
              (SentenceWindow{.begin = 4, .end = 6, .context_begin = 3, .context_end = 9}));
    // After it, the same.
    std::vector<std::string> after(9, "Radium glows.");
    after[6] = std::string(600, 'x');
    after[7] = std::string(600, 'x');
    EXPECT_EQ(window_of(transcript_of(after), 1, 6),
              (SentenceWindow{.begin = 1, .end = 6, .context_begin = 0, .context_end = 7}));
}

// --- Planned against an engine --------------------------------------------------

// The mission that reads `window` of `transcript`: its sentences with their
// context, and an answer of 64 tokens.
MissionFor reader_of(const knowledge::Transcript& transcript) {
    return [&transcript](const SentenceWindow& window, std::size_t number) -> Result<agents::Mission> {
        return agents::Mission{
            .id = std::format("read-{}", number),
            .kind = "read",
            .role = "commander",
            .request =
                llm::ChatRequest{.messages = {llm::Message::system("Read the sentences."),
                                              llm::Message::user(sentence_lines(
                                                  transcript, window.context_begin, window.context_end))}},
            .answer_tokens = 64};
    };
}

std::vector<std::string> ids_of(const WindowPlan& plan) {
    std::vector<std::string> ids;
    ids.reserve(plan.missions.size());
    for (const agents::Mission& mission : plan.missions) {
        ids.push_back(mission.id);
    }
    return ids;
}

// Every sentence in exactly one window, of `windows` or of `too_long`, in order.
void expect_each_sentence_once(const WindowPlan& plan, const knowledge::Transcript& transcript) {
    std::vector<SentenceWindow> all = plan.windows;
    all.insert(all.end(), plan.too_long.begin(), plan.too_long.end());
    std::ranges::sort(all, {}, &SentenceWindow::begin);
    std::size_t next = 0;
    for (const SentenceWindow& window : all) {
        EXPECT_EQ(window.begin, next);
        EXPECT_LT(window.begin, window.end);
        next = window.end;
    }
    EXPECT_EQ(next, transcript.sentences.size());
}

TEST(SentenceWindows, AreFilledToTheirBytesWhenTheirEngineHoldsThem) {
    const knowledge::Transcript transcript = glowing();
    const IdleArmy rig{8'192};
    const WindowPlan plan = unwrap(plan_windows(transcript, 72, rig.army(), reader_of(transcript)));
    EXPECT_EQ(plan.windows,
              (std::vector<SentenceWindow>{window_from(transcript, 0, 72), window_from(transcript, 3, 72),
                                           window_from(transcript, 6, 72), window_from(transcript, 9, 72)}));
    EXPECT_EQ(ids_of(plan), (std::vector<std::string>{"read-1", "read-2", "read-3", "read-4"}));
    EXPECT_EQ(plan.missions[1].request.messages.back().text(), sentence_lines(transcript, 0, 9))
        << "each its own window's mission";
    EXPECT_TRUE(plan.too_long.empty());
    EXPECT_TRUE(
        unwrap(plan_windows(knowledge::Transcript{}, 72, rig.army(), reader_of(transcript))).windows.empty());
}

TEST(SentenceWindows, GiveUpSentencesFromTheirEndUntilTheirEngineHoldsThem) {
    const knowledge::Transcript transcript = uneven();
    const MissionFor reader = reader_of(transcript);
    const IdleArmy rig{400};
    const WindowPlan plan = unwrap(plan_windows(transcript, 100'000, rig.army(), reader));
    ASSERT_GT(plan.windows.size(), 1U) << "one window would hold every sentence, were it not for the engine";
    EXPECT_TRUE(plan.too_long.empty());
    expect_each_sentence_once(plan, transcript);
    for (std::size_t index = 0; index < plan.windows.size(); ++index) {
        const SentenceWindow& window = plan.windows[index];
        EXPECT_EQ(plan.missions[index].id, std::format("read-{}", index + 1));
        EXPECT_TRUE(rig.army().holds(plan.missions[index])) << plan.missions[index].id;
        if (window.end < transcript.sentences.size()) {
            const agents::Mission larger =
                unwrap(reader(window_of(transcript, window.begin, window.end + 1), index + 1));
            EXPECT_FALSE(rig.army().holds(larger)) << plan.missions[index].id << ": the largest it holds";
        }
    }
}

// What plan_windows() finds by bisection, found the slow way: from the
// window filled to `bytes`, its last sentence given up while its engine
// does not hold it.
WindowPlan given_up_one_by_one(const knowledge::Transcript& transcript, std::size_t bytes,
                               const agents::Army& army, const MissionFor& mission_for) {
    WindowPlan plan;
    for (std::size_t begin = 0; begin < transcript.sentences.size();) {
        const std::size_t number = plan.missions.size() + 1;
        SentenceWindow window = window_from(transcript, begin, bytes);
        agents::Mission mission = unwrap(mission_for(window, number));
        while (!army.holds(mission) && window.end - window.begin > 1) {
            window = window_of(transcript, begin, window.end - 1);
            mission = unwrap(mission_for(window, number));
        }
        if (army.holds(mission)) {
            plan.windows.push_back(window);
            plan.missions.push_back(std::move(mission));
        } else {
            plan.too_long.push_back(window);
        }
        begin = window.end;
    }
    return plan;
}

TEST(SentenceWindows, BisectionFindsTheWindowsThatGivingUpOneSentenceAtATimeWould) {
    std::vector<std::string> texts;
    for (const knowledge::Sentence& sentence : uneven().sentences) {
        texts.push_back(sentence.text);
    }
    texts[11] = std::string(1'200, 'x');  // longer than a context holds
    texts[20] = std::string(2'000, 'y');  // longer than the smaller engines hold
    const knowledge::Transcript transcript = transcript_of(texts);
    const MissionFor reader = reader_of(transcript);
    std::size_t given_up = 0;   // windows smaller than their bytes
    std::size_t set_aside = 0;  // sentences too long even alone
    for (const int holds : {200, 300, 400, 650, 900, 2'000}) {
        for (const std::size_t bytes : {std::size_t{100}, std::size_t{700}, std::size_t{100'000}}) {
            SCOPED_TRACE(std::format("{} tokens, windows of {} bytes", holds, bytes));
            const IdleArmy rig{holds};
            const WindowPlan fast = unwrap(plan_windows(transcript, bytes, rig.army(), reader));
            const WindowPlan slow = given_up_one_by_one(transcript, bytes, rig.army(), reader);
            EXPECT_EQ(fast.windows, slow.windows);
            EXPECT_EQ(fast.too_long, slow.too_long);
            EXPECT_EQ(ids_of(fast), ids_of(slow));
            expect_each_sentence_once(fast, transcript);
            given_up += static_cast<std::size_t>(
                std::ranges::count_if(fast.windows, [&](const SentenceWindow& window) {
                    return window.end < window_from(transcript, window.begin, bytes).end;
                }));
            set_aside += fast.too_long.size();
        }
    }
    EXPECT_GT(given_up, 0U) << "the engines are small enough to give sentences up";
    EXPECT_GT(set_aside, 0U) << "and to set some aside";
}

TEST(SentenceWindows, AreReadWithTheContextAStageAsksFor) {
    const knowledge::Transcript transcript = glowing();
    const IdleArmy rig{8'192};
    const WindowPlan plan = unwrap(plan_windows(transcript, 72, rig.army(), reader_of(transcript), 1));
    ASSERT_EQ(plan.windows.size(), 4U);
    EXPECT_EQ(plan.windows[1], (SentenceWindow{.begin = 3, .end = 6, .context_begin = 2, .context_end = 7}))
        << "one sentence on either side";
}

TEST(SentenceWindows, SetAsideASentenceTooLongForItsEngineEvenAlone) {
    std::vector<std::string> texts(6, "Radium glows.");
    texts[2] = std::string(3'000, 'x');
    const knowledge::Transcript transcript = transcript_of(texts);
    const IdleArmy rig{400};
    const WindowPlan plan = unwrap(plan_windows(transcript, 100'000, rig.army(), reader_of(transcript)));
    EXPECT_EQ(plan.too_long, (std::vector<SentenceWindow>{window_of(transcript, 2, 3)}));
    EXPECT_EQ(plan.windows,
              (std::vector<SentenceWindow>{window_of(transcript, 0, 2), window_of(transcript, 3, 6)}));
    EXPECT_EQ(ids_of(plan), (std::vector<std::string>{"read-1", "read-2"}))
        << "a window set aside takes no number";
}

TEST(SentenceWindows, PassOnTheErrorOfAMissionTheyCannotBuild) {
    const knowledge::Transcript transcript = glowing();
    const IdleArmy rig{8'192};
    const MissionFor reader = reader_of(transcript);
    const MissionFor broken = [&](const SentenceWindow& window,
                                  std::size_t number) -> Result<agents::Mission> {
        if (number == 2) {
            return fail(Errc::invalid_argument, "the prompt has no 'sentences'");
        }
        return reader(window, number);
    };
    const Result<WindowPlan> plan = plan_windows(transcript, 72, rig.army(), broken);
    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().message(), "the prompt has no 'sentences'");

    // And while giving up sentences: thirty lines are more than 200 tokens.
    const knowledge::Transcript longer = glowing(30);
    const MissionFor longer_reader = reader_of(longer);
    const IdleArmy small{200};
    const MissionFor broken_trial = [&](const SentenceWindow& window,
                                        std::size_t number) -> Result<agents::Mission> {
        if (window.end - window.begin < 25) {
            return fail(Errc::invalid_argument, "the prompt has no 'sentences'");
        }
        return longer_reader(window, number);
    };
    ASSERT_FALSE(small.army().holds(unwrap(longer_reader(window_from(longer, 0, 100'000), 1))));
    EXPECT_FALSE(plan_windows(longer, 100'000, small.army(), broken_trial).has_value());
}

}  // namespace
}  // namespace rr::pipeline

// How a video's sections are made and joined: from chapters, from pauses,
// from outlines agreed on; and how the briefs of its sections become one.
#include <rr/pipeline/outline.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>

namespace rr::pipeline {
namespace {

using knowledge::Brief;
using knowledge::Genre;
using knowledge::LexiconTerm;
using knowledge::Speaker;
using testkit::unwrap;

// The transcript of sentences said over `spans`, in seconds.
knowledge::Transcript timed(const std::vector<std::pair<int, int>>& spans) {
    knowledge::Transcript transcript;
    transcript.sentences.reserve(spans.size());
    for (std::size_t index = 0; index < spans.size(); ++index) {
        transcript.sentences.push_back(
            knowledge::Sentence{.id = knowledge::SentenceId{static_cast<std::uint32_t>(index + 1)},
                                .text = "Something was said.",
                                .span = unwrap(TimeSpan::make(Millis{spans[index].first * 1'000},
                                                              Millis{spans[index].second * 1'000}))});
    }
    return transcript;
}

// `count` sentences of `seconds` each, one after the other without a pause.
knowledge::Transcript even(std::size_t count, int seconds) {
    std::vector<std::pair<int, int>> spans;
    spans.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const int start = static_cast<int>(index) * seconds;
        spans.emplace_back(start, start + seconds);
    }
    return timed(spans);
}

media::Chapter chapter(std::string title, int start, int end) {
    return media::Chapter{.title = std::move(title),
                          .span = unwrap(TimeSpan::make(Millis{start * 1'000}, Millis{end * 1'000}))};
}

std::vector<std::size_t> firsts(const std::vector<Cut>& cuts) {
    std::vector<std::size_t> starts;
    starts.reserve(cuts.size());
    for (const Cut& cut : cuts) {
        starts.push_back(cut.first);
    }
    return starts;
}

std::vector<std::string> titles(const std::vector<Cut>& cuts) {
    std::vector<std::string> named;
    named.reserve(cuts.size());
    for (const Cut& cut : cuts) {
        named.push_back(cut.title);
    }
    return named;
}

// --- Chapters ------------------------------------------------------------------

TEST(Sections, AChapterStartsAtTheSentenceThatStartsClosestToItsTime) {
    const knowledge::Transcript transcript = even(6, 10);  // S1 at 0:00, S2 at 0:10, … S6 at 0:50
    const std::vector<media::Chapter> chapters{chapter("Intro", 8, 19), chapter("Polonium", 19, 44),
                                               chapter("Radium", 44, 60)};
    EXPECT_EQ(chapter_cuts(chapters, transcript), (std::vector<Cut>{{.first = 0, .title = "Intro"},
                                                                    {.first = 2, .title = "Polonium"},
                                                                    {.first = 4, .title = "Radium"}}))
        << "the first from the first sentence, though 0:08 is nearer S2; 0:19 is S3's, 0:44 is S5's";
    EXPECT_EQ(
        firsts(chapter_cuts(std::vector{chapter("Intro", 0, 45), chapter("Radium", 45, 60)}, transcript)),
        (std::vector<std::size_t>{0, 4}))
        << "0:45 is as close to S5 as to S6: the earlier";
}

TEST(Sections, AChapterThatWouldStartWhereOrBeforeTheOneBeforeIsDropped) {
    const knowledge::Transcript transcript = even(6, 10);
    const std::vector<media::Chapter> chapters{chapter("Intro", 0, 2), chapter("Welcome", 2, 30),
                                               chapter("Polonium", 30, 40), chapter("Out of order", 12, 20),
                                               chapter("Radium", 41, 60)};
    EXPECT_EQ(titles(chapter_cuts(chapters, transcript)),
              (std::vector<std::string>{"Intro", "Polonium", "Radium"}));
    EXPECT_TRUE(chapter_cuts(chapters, knowledge::Transcript{}).empty());
    EXPECT_TRUE(chapter_cuts({}, transcript).empty());
}

TEST(Sections, KnowHowLongTheyRun) {
    const knowledge::Transcript transcript = timed({{0, 10}, {12, 20}, {25, 31}, {40, 52}});
    const std::vector<Cut> cuts{{.first = 0, .title = "A"}, {.first = 2, .title = "B"}};
    EXPECT_EQ(section_length(cuts, 0, transcript), Millis{20'000}) << "from S1's start to S2's end";
    EXPECT_EQ(section_length(cuts, 1, transcript), Millis{27'000}) << "the last runs to the transcript's end";
}

// --- Long sections ----------------------------------------------------------------

TEST(Sections, ALongOneIsCutWhereTheSpeakerPausedLongestLeavingEachPartAQuarter) {
    // Two minutes; the longest pause (20 s, before S2) would leave a part of
    // a twelfth. Among the balanced cuts, S6 follows the longest pause (3 s).
    const knowledge::Transcript transcript = timed({{0, 10},
                                                    {30, 40},
                                                    {40, 60},
                                                    {60, 80},
                                                    {80, 100},
                                                    {103, 120},
                                                    {120, 140},
                                                    {140, 160},
                                                    {160, 180},
                                                    {180, 200}});
    const std::vector<Cut> parts =
        cut_long(std::vector<Cut>{{.first = 0, .title = "Radium"}}, transcript, Millis{150'000});
    EXPECT_EQ(parts, (std::vector<Cut>{{.first = 0, .title = "Radium (1 of 2)"},
                                       {.first = 5, .title = "Radium (2 of 2)"}}));
}

TEST(Sections, BetweenPausesAsLongALongOneIsCutNearestItsMiddle) {
    const knowledge::Transcript transcript = even(10, 10);  // no pause at all
    EXPECT_EQ(firsts(cut_long(std::vector<Cut>{{.first = 0, .title = "Radium"}}, transcript, Millis{60'000})),
              (std::vector<std::size_t>{0, 5}))
        << "at 0:50 of 1:40, not at the first balanced cut";
    // Two cuts as near the middle: the earlier. Three sentences of 0:10 make
    // 0:30; cutting before S2 leaves 0:10 and 0:20, before S3 0:20 and 0:10.
    EXPECT_EQ(
        firsts(cut_long(std::vector<Cut>{{.first = 0, .title = "Radium"}}, even(3, 10), Millis{25'000})),
        (std::vector<std::size_t>{0, 1}));
    // No cut leaves a quarter: nearest the middle, the earlier of two as near.
    const knowledge::Transcript lopsided = timed({{0, 10}, {10, 100}, {100, 110}});
    EXPECT_EQ(firsts(cut_long(std::vector<Cut>{{.first = 0, .title = "Radium"}}, lopsided, Millis{60'000})),
              (std::vector<std::size_t>{0, 1, 2}));
}

TEST(Sections, ALongOneIsCutAgainUntilNoPartIsLongerAndASentenceIsNeverCut) {
    const knowledge::Transcript transcript = even(12, 10);
    EXPECT_EQ(cut_long(std::vector<Cut>{{.first = 0, .title = "Radium"}}, transcript, Millis{50'000}),
              (std::vector<Cut>{{.first = 0, .title = "Radium (1 of 4)"},
                                {.first = 3, .title = "Radium (2 of 4)"},
                                {.first = 6, .title = "Radium (3 of 4)"},
                                {.first = 9, .title = "Radium (4 of 4)"}}));
    const knowledge::Transcript one = timed({{0, 600}});
    EXPECT_EQ(cut_long(std::vector<Cut>{{.first = 0, .title = "Radium"}}, one, Millis{60'000}),
              (std::vector<Cut>{{.first = 0, .title = "Radium"}}))
        << "a sentence of ten minutes is one section";
}

TEST(Sections, OnlyTheLongOnesAreCut) {
    const knowledge::Transcript transcript = even(10, 10);
    EXPECT_EQ(cut_long(std::vector<Cut>{{.first = 0, .title = "Intro"}, {.first = 2, .title = "Radium"}},
                       transcript, Millis{50'000}),
              (std::vector<Cut>{{.first = 0, .title = "Intro"},
                                {.first = 2, .title = "Radium (1 of 2)"},
                                {.first = 6, .title = "Radium (2 of 2)"}}))
        << "Radium runs 0:20 to 1:40; its middle is at 1:00";
    EXPECT_TRUE(cut_long({}, transcript).empty());
}

// --- Pauses ------------------------------------------------------------------------

TEST(Sections, WithoutChaptersOrAModelAreCutAtPausesAndTitledByTheirStretch) {
    const knowledge::Transcript transcript = even(10, 60);  // ten minutes
    EXPECT_EQ(pause_cuts(transcript, 0, 10), (std::vector<Cut>{{.first = 0, .title = "0:00\u20135:00"},
                                                               {.first = 5, .title = "5:00\u201310:00"}}));
    EXPECT_EQ(pause_cuts(transcript, 2, 4), (std::vector<Cut>{{.first = 2, .title = "2:00\u20134:00"}}))
        << "a stretch of the transcript";
    EXPECT_TRUE(pause_cuts(transcript, 3, 3).empty());
    EXPECT_TRUE(pause_cuts(knowledge::Transcript{}, 0, 0).empty());
}

// --- Agreement --------------------------------------------------------------------

TEST(Sections, OutlinesAgreeWhereEnoughOfThemStartOneNearEachOther) {
    const std::vector<std::vector<Cut>> outlines{
        {{.first = 0, .title = "Intro"}, {.first = 4, .title = "Polonium"}, {.first = 10, .title = "Radium"}},
        {{.first = 0, .title = "Opening"},
         {.first = 5, .title = "Polonium found"},
         {.first = 10, .title = "Radium found"},
         {.first = 15, .title = "Thanks"}},
        {{.first = 0, .title = "Start"},
         {.first = 6, .title = "Polonium again"},
         {.first = 11, .title = "Radium again"}},
    };
    EXPECT_EQ(agreed_cuts(outlines, 1, 0.5), (std::vector<Cut>{{.first = 0, .title = "Intro"},
                                                               {.first = 5, .title = "Polonium found"},
                                                               {.first = 10, .title = "Radium"}}))
        << "S6 is between S5 and S7; the thanks are one outline's; titles from the first outline that starts "
           "there";
    EXPECT_EQ(agreed_cuts(outlines, 0, 1.0), (std::vector<Cut>{{.first = 0, .title = "Intro"}}))
        << "all of them, exactly";
    EXPECT_EQ(agreed_cuts(std::span(outlines).first(1), 2, 0.5), outlines[0])
        << "one outline agrees with itself";
    EXPECT_TRUE(agreed_cuts({}, 2, 0.5).empty());
}

// --- Briefs -------------------------------------------------------------------------

TEST(Briefs, JoinTheGenreMostOfThemName) {
    const std::vector<Brief> briefs{Brief{.genre = Genre::conversation}, Brief{.genre = Genre::talk},
                                    Brief{.genre = Genre::talk}};
    EXPECT_EQ(join_briefs(briefs).genre, Genre::talk);
    const std::vector<Brief> tied{Brief{.genre = Genre::tutorial}, Brief{.genre = Genre::talk}};
    EXPECT_EQ(join_briefs(tied).genre, Genre::tutorial) << "the first named on a tie";
    EXPECT_EQ(join_briefs({}), Brief{}) << "an other video with nothing in its brief";
}

TEST(Briefs, JoinTheirSpeakersAndWordsEachOnceARoleOrMeaningFillingAnEmptyOne) {
    const std::vector<Brief> briefs{
        Brief{.speakers = {Speaker{.name = "Ada Lovelace"}, Speaker{.name = "Ben", .role = "host"}},
              .lexicon = {LexiconTerm{.term = "Proof load"}}},
        Brief{.speakers = {Speaker{.name = "ADA LOVELACE", .role = "guest"},
                           Speaker{.name = "Ben", .role = "guest"}},
              .lexicon = {LexiconTerm{.term = "proof load", .meaning = "the most a bridge carries"},
                          LexiconTerm{.term = "arch", .meaning = "a curve that only pushes"}}},
    };
    const Brief joined = join_briefs(briefs);
    EXPECT_EQ(joined.speakers, (std::vector<Speaker>{Speaker{.name = "Ada Lovelace", .role = "guest"},
                                                     Speaker{.name = "Ben", .role = "host"}}))
        << "as first written; a role fills an empty one, and never replaces one";
    EXPECT_EQ(joined.lexicon, (std::vector<LexiconTerm>{
                                  LexiconTerm{.term = "Proof load", .meaning = "the most a bridge carries"},
                                  LexiconTerm{.term = "arch", .meaning = "a curve that only pushes"}}));
    EXPECT_FALSE(joined.gist.has_value()) << "the gist is the caller's";
}

TEST(Briefs, KeepTheWordsMostOfThemNameWhenTheyNameTooMany) {
    // Section 1 lists 40 words of its own and "radium"; sections 2 and 3
    // list "radium" and "polonium", twice over.
    Brief first;
    for (std::size_t index = 0; index < kMostTerms; ++index) {
        first.lexicon.push_back(LexiconTerm{.term = std::format("word {}", index + 1)});
    }
    first.lexicon.push_back(LexiconTerm{.term = "radium"});
    const Brief again{.lexicon = {LexiconTerm{.term = "radium"}, LexiconTerm{.term = "polonium"},
                                  LexiconTerm{.term = "Polonium"}}};
    const Brief joined = join_briefs(std::vector<Brief>{first, again, again});
    ASSERT_EQ(joined.lexicon.size(), kMostTerms);
    EXPECT_EQ(joined.lexicon[0].term, "word 1") << "in the order first named";
    EXPECT_EQ(joined.lexicon[kMostTerms - 3].term, "word 38") << "the last two of the first's own made room";
    EXPECT_EQ(joined.lexicon[kMostTerms - 2].term, "radium") << "named three times";
    EXPECT_EQ(joined.lexicon[kMostTerms - 1].term, "polonium") << "named twice, once by each brief";

    // A brief names a word once, however often it lists it.
    Brief listing;
    for (std::size_t index = 0; index < kMostTerms; ++index) {
        listing.lexicon.push_back(LexiconTerm{.term = std::format("term {}", index + 1)});
    }
    for (int time = 0; time < 3; ++time) {
        listing.lexicon.push_back(LexiconTerm{.term = "repeated"});
    }
    const Brief shared{.lexicon = {LexiconTerm{.term = "shared"}}};
    const Brief kept = join_briefs(std::vector<Brief>{listing, shared, shared});
    EXPECT_TRUE(std::ranges::contains(kept.lexicon, LexiconTerm{.term = "shared"})) << "named by two briefs";
    EXPECT_FALSE(std::ranges::contains(kept.lexicon, LexiconTerm{.term = "repeated"}))
        << "listed three times by one brief, named by one";

    Brief crowd;
    for (std::size_t index = 0; index < kMostSpeakers + 3; ++index) {
        crowd.speakers.push_back(Speaker{.name = std::format("Speaker {}", index + 1)});
    }
    EXPECT_EQ(join_briefs(std::vector<Brief>{crowd}).speakers.size(), kMostSpeakers);
}

// --- The outline ------------------------------------------------------------------

TEST(Sections, MakeTheOutlineOfTheTranscript) {
    const knowledge::Transcript transcript = even(6, 10);
    const knowledge::Outline outline =
        outline_of(std::vector<Cut>{{.first = 0, .title = "Intro"}, {.first = 2, .title = "Radium"}},
                   transcript, Brief{.genre = Genre::talk}, knowledge::SectionsFrom::chapters);
    EXPECT_EQ(outline, (knowledge::Outline{.sections = {knowledge::Section{.first = knowledge::SentenceId{1},
                                                                           .last = knowledge::SentenceId{2},
                                                                           .title = "Intro"},
                                                        knowledge::Section{.first = knowledge::SentenceId{3},
                                                                           .last = knowledge::SentenceId{6},
                                                                           .title = "Radium"}},
                                           .brief = Brief{.genre = Genre::talk},
                                           .sections_from = knowledge::SectionsFrom::chapters}));
    EXPECT_TRUE(knowledge::outline_problems(outline, transcript).empty());
}

}  // namespace
}  // namespace rr::pipeline

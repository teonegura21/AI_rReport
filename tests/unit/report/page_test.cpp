// The page of a job on the synthetic lecture (tests/unit/pipeline/lecture.hpp):
// each sentence starting the player at its moment, markers as links to atoms,
// the Fidelity Report's figures checked again, what is left out and why,
// and nothing from the video taken as HTML.
#include <rr/report/page.hpp>

#include <chrono>
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/core/strings.hpp>

#include "../pipeline/lecture.hpp"

namespace rr::report {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;

// The lecture's job, done: its artifacts as the oracle makes them, and its
// article as the oracle writes it, each paragraph its atoms' words.
Contents lecture_contents() {
    Contents contents{.job = "J1", .tier = "max"};
    contents.video = pipeline::testing::source().video;
    contents.video->id = "dQw4w9WgXcQ";
    contents.transcript = pipeline::testing::lecture();
    contents.outline = pipeline::testing::lecture_outline();
    contents.ledger = pipeline::testing::lecture_ledger();
    contents.plan = pipeline::testing::lecture_plan();
    contents.article = knowledge::Article{.markdown = pipeline::testing::oracle_article()};
    return contents;
}

TEST(Escaped, EscapesWhatHtmlReads) {
    EXPECT_EQ(escaped(R"(<a href="x">Tom & Jerry's</a>)"),
              "&lt;a href=&quot;x&quot;&gt;Tom &amp; Jerry&#39;s&lt;/a&gt;");
    EXPECT_EQ(escaped("plain words"), "plain words");
}

TEST(Page, EverySentenceStartsThePlayerWhereItsFirstAtomWasSaid) {
    const std::string page = render_page(lecture_contents());
    EXPECT_THAT(page,
                HasSubstr(R"(<span class="s" data-atoms="A1" data-t="30">I&#39;m Ada, and today we weigh )"
                          R"(radium.<a class="cite" href="#A1">A1</a></span>)"));
    EXPECT_THAT(page, HasSubstr(R"(<iframe id="player" data-src="https://www.youtube-nocookie.com/embed/)"
                                R"(dQw4w9WgXcQ?rel=0")"));
}

TEST(Page, ASentenceOfSeveralAtomsStartsAtItsFirst) {
    Contents contents = lecture_contents();
    contents.article =
        knowledge::Article{.markdown = "## Weighing radium\n\nThe NRC lent Ada 3 scales. [A2][A1]\n"};
    EXPECT_THAT(render_page(contents),
                HasSubstr(R"(<span class="s" data-atoms="A2 A1" data-t="90">The NRC lent Ada )"
                          R"(3 scales.<a class="cite" href="#A2">A2</a><a class="cite" )"
                          R"(href="#A1">A1</a></span>)"));
}

TEST(Page, TheVideoIsNamedLinkedAndPlayedAndTheJobSaid) {
    Contents contents = lecture_contents();
    contents.video->duration = std::chrono::minutes{15};
    EXPECT_THAT(
        render_page(contents),
        HasSubstr("<h1>Weighing radium</h1>\n<p class=\"meta\">Science Hour \u00b7 15:00 \u00b7 <a "
                  "href=\"https://www.youtube.com/watch?v=dQw4w9WgXcQ\">the video</a> \u00b7 job J1, tier "
                  "max</p>"));
}

TEST(Page, EachSectionIsHeadedWithItsStartAndListedInTheContents) {
    const std::string page = render_page(lecture_contents());
    EXPECT_THAT(page,
                HasSubstr(R"(<h2 id="section-2">The samples <a class="time" )"
                          R"(href="https://www.youtube.com/watch?v=dQw4w9WgXcQ&amp;t=300s" data-t="300">)"
                          R"(5:00</a></h2>)"));
    EXPECT_THAT(page, HasSubstr(R"(<li><a href="#section-2">The samples</a> <a class="time")"));
}

TEST(Page, ACaptionFilesMomentsLinkToItsTranscript) {
    Contents contents = lecture_contents();
    contents.video->id.clear();
    const std::string page = render_page(contents);
    EXPECT_THAT(page,
                HasSubstr(R"(<h2 id="section-2">The samples <a class="time" href="#S11" data-t="300">)"));
    EXPECT_THAT(page, HasSubstr(R"(<tr id="S11"><td>S11</td>)"));
    EXPECT_THAT(page, Not(HasSubstr("youtube")));
}

TEST(Page, TheFidelityReportChecksTheArtifactsAgain) {
    const std::string page = render_page(lecture_contents());
    EXPECT_THAT(
        page, HasSubstr("<tr><td>Sentences accounted for</td><td>100.0%</td><td>30 of 30</td><td>all</td>"));
    EXPECT_THAT(page,
                HasSubstr("<tr><td>Atoms told</td><td>100.0%</td><td>26 of 26</td><td>97% or more</td>"));
    EXPECT_THAT(page, HasSubstr("<tr><td>Invalid markers</td><td>0</td><td>of 26 written</td><td>none</td>"));
}

TEST(Page, WhatWasKeptIsCountedOnTheArticleAsItIs) {
    Contents contents = lecture_contents();
    contents.article->markdown = replace_all(contents.article->markdown, "Its half-life is long. [A4] ", "");
    EXPECT_THAT(render_page(contents), HasSubstr("<tr><td>Atoms told</td><td>96.2%</td><td>25 of 26</td>"));
}

TEST(Page, TheLedgerShowsEachAtomsTimeLabelsWordsAndWhoSaidIt) {
    Contents contents = lecture_contents();
    contents.ledger->atoms.at(2).speaker = "Ada";
    EXPECT_THAT(
        render_page(contents),
        HasSubstr(R"(<tr id="A3"><td>A3</td><td><a class="time" href="https://www.youtube.com/watch?v=)"
                  R"(dQw4w9WgXcQ&amp;t=120s" data-t="120">2:00</a></td><td>claim, hedged</td><td>Maybe the )"
                  R"(scales are older than the lab. <em>(Ada)</em></td></tr>)"));
}

TEST(Page, WhatIsNotToldIsListedWithWhy) {
    Contents contents = lecture_contents();
    contents.plan->duplicates = {knowledge::Duplicate{
        .atom = knowledge::AtomId{5}, .of = knowledge::AtomId{4}, .because = "Said again."}};
    const std::string page = render_page(contents);
    EXPECT_THAT(page, HasSubstr("<td>greeting</td><td>Hello and welcome back.</td>"));
    EXPECT_THAT(page, HasSubstr(R"(<td><a href="#A5">A5</a></td><td>says again <a href="#A4">A4</a></td>)"
                                R"(<td>Said again.</td>)"));
}

TEST(Page, WhatTheStagesCouldNotDoIsShown) {
    Contents contents = lecture_contents();
    contents.findings = {
        StageFindings{.stage = "source"},
        StageFindings{.stage = "write", .findings = {"section 2: told as its atoms say it"}}};
    const std::string page = render_page(contents);
    EXPECT_THAT(page, HasSubstr("<h4>write</h4>\n<ul><li>section 2: told as its atoms say it</li></ul>"));
    EXPECT_THAT(page, Not(HasSubstr("<h4>source</h4>"))) << "a stage that did all it was asked is not listed";

    contents.findings = {StageFindings{.stage = "source"}, StageFindings{.stage = "write"}};
    EXPECT_THAT(render_page(contents), HasSubstr("<p>Nothing: every stage did all it was asked.</p>"));
}

TEST(Page, AJobNotDoneShowsWhatItHasAndSaysWhatIsMissing) {
    Contents contents = lecture_contents();
    contents.article.reset();
    const std::string written = render_page(contents);
    EXPECT_THAT(written, HasSubstr("The article is not written yet"));
    EXPECT_THAT(written, HasSubstr("The ledger: 26 atoms"));
    contents.ledger.reset();
    contents.plan.reset();
    EXPECT_THAT(render_page(contents), HasSubstr("Nothing yet: the job has not written Ledger A."));
    EXPECT_THAT(render_page(contents), HasSubstr("The transcript: 30 sentences"));
}

TEST(Page, AJobBeforeItsSourceIsAPageToo) {
    const std::string page = render_page(Contents{.job = "J1", .tier = "fast"});
    EXPECT_THAT(page, HasSubstr("<title>A video without a title</title>"));
    EXPECT_THAT(page, HasSubstr("<p class=\"meta\">job J1, tier fast</p>"));
    EXPECT_THAT(page, HasSubstr("The article is not written yet"));
    EXPECT_THAT(page, Not(HasSubstr("<details>")));
}

TEST(Page, NothingFromTheVideoIsTakenAsHtml) {
    Contents contents = lecture_contents();
    contents.video->title = "<script>alert(1)</script>";
    contents.video->channel = "Science <i>Hour</i>";
    contents.transcript->sentences.front().text = "Hello <u>and</u> welcome back.";
    contents.ledger->atoms.front().text = "Tom & <b>Jerry</b>";
    contents.ledger->atoms.front().speaker = R"(Ada "the scale" <Lovelace>)";
    contents.article->markdown =
        replace_all(contents.article->markdown, "Why does it glow?", "Why does <it> glow?");
    contents.findings = {StageFindings{.stage = "<write>", .findings = {"section 2: <told> as said"}}};
    const std::string page = render_page(contents);
    EXPECT_THAT(page, Not(HasSubstr("<script>alert")));
    EXPECT_THAT(page, HasSubstr("<title>&lt;script&gt;alert(1)&lt;/script&gt;</title>"));
    EXPECT_THAT(page, HasSubstr("Science &lt;i&gt;Hour&lt;/i&gt;"));
    EXPECT_THAT(page, HasSubstr("Hello &lt;u&gt;and&lt;/u&gt; welcome back."));
    EXPECT_THAT(page, HasSubstr("Tom &amp; &lt;b&gt;Jerry&lt;/b&gt;"));
    EXPECT_THAT(page, HasSubstr("Ada &quot;the scale&quot; &lt;Lovelace&gt;"));
    EXPECT_THAT(page, HasSubstr("Why does &lt;it&gt; glow?")) << "the article, a model's words";
    EXPECT_THAT(page, HasSubstr("<h4>&lt;write&gt;</h4>\n<ul><li>section 2: &lt;told&gt; as said</li></ul>"));
}

TEST(Page, OnlyAnIdOfYouTubesFormMakesAVideoLink) {
    Contents contents = lecture_contents();
    contents.video->id = R"(x"><script>)";
    EXPECT_THAT(render_page(contents), Not(HasSubstr("youtube")));
    contents.video->id = "dQw4w9WgXc";  // ten
    EXPECT_THAT(render_page(contents), Not(HasSubstr("youtube")));
}

}  // namespace
}  // namespace rr::report

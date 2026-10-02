#include <rr/knowledge/article.hpp>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <rr/json/json.hpp>
#include <rr/testkit/result.hpp>

namespace rr::knowledge {
namespace {

using testkit::unwrap;
using Texts = std::vector<std::string>;

// Each sentence of every block, as its text and the atoms it cites.
using Cited = std::vector<std::pair<std::string, std::vector<std::uint32_t>>>;

Cited cited_by_sentence(std::string_view markdown) {
    const ParsedArticle parsed = parse_article(markdown);
    Cited found;
    for (const Block& block : parsed.blocks) {
        for (const CitingSentence& sentence : block.sentences) {
            std::vector<std::uint32_t> atoms;
            atoms.reserve(sentence.markers.size());
            for (const Marker& marker : sentence.markers) {
                atoms.push_back(marker.atom.number());
            }
            found.emplace_back(std::string{text::text_of(markdown, sentence.span)}, std::move(atoms));
        }
    }
    return found;
}

// Each bad marker, as what was written and why it is bad.
std::vector<std::pair<std::string, std::string>> bad_markers(std::string_view markdown) {
    std::vector<std::pair<std::string, std::string>> found;
    for (const BadMarker& bad : parse_article(markdown).bad_markers) {
        found.emplace_back(std::string{text::text_of(markdown, bad.span)}, bad.reason);
    }
    return found;
}

std::vector<BlockKind> kinds(std::string_view markdown) {
    std::vector<BlockKind> found;
    for (const Block& block : parse_article(markdown).blocks) {
        found.push_back(block.kind);
    }
    return found;
}

TEST(Article, SentencesCiteTheAtomsOfTheirMarkers) {
    EXPECT_EQ(cited_by_sentence("He hedges: \"I could be wrong\" [A42]. The data came first [A41][A43]."),
              (Cited{{"He hedges: \"I could be wrong\" [A42].", {42}},
                     {"The data came first [A41][A43].", {41, 43}}}));
}

TEST(Article, MarkersKnowWhereTheyAre) {
    constexpr std::string_view kText = "Water boils [A12].";
    const ParsedArticle parsed = parse_article(kText);
    ASSERT_EQ(parsed.blocks.size(), 1U);
    ASSERT_EQ(parsed.blocks[0].sentences.size(), 1U);
    const std::vector<Marker>& markers = parsed.blocks[0].sentences[0].markers;
    ASSERT_EQ(markers.size(), 1U);
    EXPECT_EQ(markers[0].atom, AtomId{12});
    EXPECT_EQ(text::text_of(kText, markers[0].span), "[A12]");
}

TEST(Article, ASentenceMayCiteNothing) {
    EXPECT_EQ(cited_by_sentence("Let us turn to the second part. It starts with water [A7]."),
              (Cited{{"Let us turn to the second part.", {}}, {"It starts with water [A7].", {7}}}));
}

TEST(Article, AMarkerAfterTheTerminatorBelongsToItsSentence) {
    EXPECT_EQ(cited_by_sentence("Water boils at sea level. [A12] Ice melts. [A13][A14]"),
              (Cited{{"Water boils at sea level. [A12]", {12}}, {"Ice melts. [A13][A14]", {13, 14}}}));
}

TEST(Article, AMarkerAtTheStartOfABlockStaysInIt) {
    EXPECT_EQ(cited_by_sentence("[A3] Water boils."), (Cited{{"[A3] Water boils.", {3}}}));
    EXPECT_EQ(cited_by_sentence("[A3]. Water boils."), (Cited{{"[A3]. Water boils.", {3}}}))
        << "a first sentence of markers alone joins the one after";
}

TEST(Article, ABlockOfMarkersAloneIsOneSentence) {
    EXPECT_EQ(cited_by_sentence("[A3][A4]"), (Cited{{"[A3][A4]", {3, 4}}}));
}

TEST(Article, CitationsThatAreNoMarkersAreBad) {
    using Bad = std::vector<std::pair<std::string, std::string>>;
    EXPECT_EQ(bad_markers("one [A41, A43] two [A41-A43] three [A41 and A43]"),
              (Bad{{"[A41, A43]", "one atom per marker: [A41][A43]"},
                   {"[A41-A43]", "one atom per marker, each atom of a range too: [A41]\u2026[A43]"},
                   {"[A41 and A43]", "one atom per marker: [A41][A43]"}}));
    EXPECT_EQ(bad_markers("one [a41] two [ A41 ] three [A041] four [A 41]"),
              (Bad{{"[a41]", "write it [A41]"},
                   {"[ A41 ]", "write it [A41]"},
                   {"[A041]", "write it [A41]"},
                   {"[A 41]", "write it [A41]"}}));
    EXPECT_EQ(bad_markers("one [S12] two [V3] three [s4]"),
              (Bad{{"[S12]", "the article cites atoms, not sentences or frames: [A#]"},
                   {"[V3]", "the article cites atoms, not sentences or frames: [A#]"},
                   {"[s4]", "the article cites atoms, not sentences or frames: [A#]"}}));
    EXPECT_EQ(bad_markers("one [A0] two [A] three [A#] four [A41 above]"),
              (Bad{{"[A0]", "atoms are numbered from A1"},
                   {"[A]", "the atom's number is missing: cite it as [A12]"},
                   {"[A#]", "the atom's number is missing: cite it as [A12]"},
                   {"[A41 above]", "a marker holds an atom's ID only: [A41]"}}));
    EXPECT_EQ(bad_markers("one [A1234567890]"), (Bad{{"[A1234567890]", "no atom has that number"}}));
}

TEST(Article, AnUnclosedMarkerIsBad) {
    EXPECT_EQ(bad_markers("Water boils [A12. Ice melts [A13]."),
              (std::vector<std::pair<std::string, std::string>>{
                  {"[A12. Ice melts ", "an unclosed marker: close it with ]"}}));
    EXPECT_EQ(
        bad_markers("At the end [A12"),
        (std::vector<std::pair<std::string, std::string>>{{"[A12", "an unclosed marker: close it with ]"}}));
}

TEST(Article, BracketsThatAreNoCitationsAreText) {
    EXPECT_TRUE(bad_markers("[Applause] [1] [see above] [a] [Sic] [Video] []").empty());
    EXPECT_EQ(cited_by_sentence("[Applause] They laughed."), (Cited{{"[Applause] They laughed.", {}}}));
}

TEST(Article, CodeHoldsNoMarkers) {
    EXPECT_EQ(cited_by_sentence("Use `x[A1]` here [A2]."), (Cited{{"Use `x[A1]` here [A2].", {2}}}));
    EXPECT_TRUE(bad_markers("Use ``a[S1]b`` freely.").empty());
    EXPECT_TRUE(bad_markers("Use ``a`[S1]`b`` freely.").empty())
        << "code closes on a run as long as it opened";
    const ParsedArticle parsed = parse_article("Code:\n\n```\nv[A1] = 2;\n```\n\nAfter [A3].");
    ASSERT_EQ(parsed.blocks.size(), 3U);
    EXPECT_EQ(parsed.blocks[1].kind, BlockKind::code);
    EXPECT_TRUE(parsed.blocks[1].sentences.empty());
    EXPECT_EQ(parsed.cited(), std::vector<AtomId>{AtomId{3}});
}

TEST(Article, AnEscapedBracketIsText) {
    EXPECT_EQ(cited_by_sentence("Written as \\[A1] here [A2]."),
              (Cited{{"Written as \\[A1] here [A2].", {2}}}));
}

TEST(Article, UnmatchedBackticksAreText) {
    EXPECT_EQ(cited_by_sentence("A lone ` here [A2]."), (Cited{{"A lone ` here [A2].", {2}}}));
}

TEST(Article, InlineCodeRunsToAnEqualRunOfBackticks) {
    const std::string_view markdown = "Use `a`, ``b ` c`` and a lone ` here.";
    std::vector<std::string_view> code;
    for (const text::Span& span : inline_code(markdown, text::Span{.begin = 0, .end = markdown.size()})) {
        code.push_back(text::text_of(markdown, span));
    }
    EXPECT_EQ(code, (std::vector<std::string_view>{"`a`", "``b ` c``"}));
    EXPECT_TRUE(inline_code(markdown, text::Span{.begin = 4, .end = 6}).empty())
        << "within \"`a\" alone, the run is never matched";
}

TEST(Article, BlocksAreMarkdowns) {
    constexpr std::string_view kText =
        "# Title [A1]\n\nA paragraph\nthat wraps [A2].\n\n- an item [A3]\n- another\n"
        "1. numbered\n\n> quoted\n> on [A4]\n\n```cpp\ncode\n```\n";
    EXPECT_EQ(kinds(kText),
              (std::vector<BlockKind>{BlockKind::heading, BlockKind::paragraph, BlockKind::list_item,
                                      BlockKind::list_item, BlockKind::list_item, BlockKind::quote,
                                      BlockKind::code}));
    EXPECT_EQ(cited_by_sentence(kText), (Cited{{"Title [A1]", {1}},
                                               {"A paragraph\nthat wraps [A2].", {2}},
                                               {"an item [A3]", {3}},
                                               {"another", {}},
                                               {"numbered", {}},
                                               {"quoted\n> on [A4]", {4}}}));
}

TEST(Article, AHeadingIsOneToSixHashesAndASpace) {
    EXPECT_EQ(kinds("#5 is a hashtag\n\n####### is too many\n\n###### Six"),
              (std::vector<BlockKind>{BlockKind::paragraph, BlockKind::paragraph, BlockKind::heading}));
}

TEST(Article, AFenceClosesOnlyWithOneAsLong) {
    EXPECT_EQ(parse_article("````\n```\nstill code [A1]\n````\n\nAfter [A2].").cited(),
              std::vector<AtomId>{AtomId{2}});
}

TEST(Article, AFenceNeverClosedRunsToTheEnd) {
    EXPECT_EQ(kinds("Text.\n\n```\nnever closed [A1].\n\nstill code"),
              (std::vector<BlockKind>{BlockKind::paragraph, BlockKind::code}));
    EXPECT_TRUE(parse_article("```\n[A1]").cited().empty());
}

TEST(Article, CarriageReturnsEndLines) {
    EXPECT_EQ(kinds("# Title\r\n\r\nText [A1].\r\n"),
              (std::vector<BlockKind>{BlockKind::heading, BlockKind::paragraph}));
    EXPECT_EQ(kinds("One [A1].\r\n\r\nTwo [A2].\r\n"),
              (std::vector<BlockKind>{BlockKind::paragraph, BlockKind::paragraph}))
        << "a blank line ended by CRLF is blank";
}

TEST(Article, CitedListsEachAtomOnceInOrder) {
    EXPECT_EQ(parse_article("B [A2]. A [A1][A2]. C [A3].").cited(),
              (std::vector<AtomId>{AtomId{2}, AtomId{1}, AtomId{3}}));
}

TEST(Article, WithoutMarkersIsWhatAReaderSees) {
    constexpr std::string_view kText = "He hedges [A42]. Data first [A41][A43]. Keep [a7].";
    const ParsedArticle parsed = parse_article(kText);
    EXPECT_EQ(without_markers(kText, parsed, text::Span{.begin = 0, .end = kText.size()}),
              "He hedges. Data first. Keep [a7].")
        << "bad markers stay: they are text until repaired";
    const text::Span second = parsed.blocks[0].sentences[1].span;
    EXPECT_EQ(without_markers(kText, parsed, second), "Data first.");
    EXPECT_EQ(
        without_markers("[A1] Starts.", parse_article("[A1] Starts."), text::Span{.begin = 0, .end = 12}),
        "Starts.");
}

TEST(Article, AnEmptyArticleHasNoBlocks) {
    EXPECT_TRUE(parse_article("").blocks.empty());
    EXPECT_TRUE(parse_article("\n\n  \n").blocks.empty());
}

TEST(Article, TheArtifactIsItsMarkdown) {
    const Article article{.markdown = "Water boils [A12]."};
    EXPECT_EQ(unwrap(article_from_json(to_json(article))), article);
    EXPECT_EQ(unwrap(article_from_json(json::Json{{"markdown", ""}})), Article{})
        << "a video with nothing to retell has an empty article";
    EXPECT_FALSE(article_from_json(json::Json{{"markdown", "x"}, {"extra", 1}}).has_value());
}

}  // namespace
}  // namespace rr::knowledge

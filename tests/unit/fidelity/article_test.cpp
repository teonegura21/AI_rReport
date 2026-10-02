#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <rr/core/strings.hpp>
#include <rr/fidelity/checks.hpp>

#include "fixture.hpp"

namespace rr::fidelity {
namespace {

using fixture::AtomId;
using fixture::AtomLedger;
using fixture::checks;
using fixture::kArticle;
using fixture::ledger;
using fixture::SentenceId;
using fixture::transcript;

// The faithful article with one change.
std::string changed(std::string_view from, std::string_view to) {
    EXPECT_NE(kArticle.find(from), std::string::npos) << from;
    return replace_all(kArticle, from, to);
}

TEST(Article, AFaithfulArticleHasNoFinding) {
    const Report report = check_article(transcript(), ledger(), kArticle);
    EXPECT_TRUE(report.findings.empty()) << report.findings.front().message;
    const Coverage& coverage = report.coverage;
    EXPECT_EQ(coverage.atoms, 4U);
    EXPECT_EQ(coverage.atoms_told, 4U);
    EXPECT_EQ(coverage.markers, 4U);
    EXPECT_EQ(coverage.markers_invalid, 0U);
    EXPECT_EQ(coverage.must_quotes, 1U);
    EXPECT_EQ(coverage.must_quotes_kept, 1U);
    EXPECT_EQ(coverage.numbers, 4U) << "100, two, 37%, 2023";
    EXPECT_EQ(coverage.numbers_kept, 4U) << "37% and 2023 in words";
}

TEST(Article, ABadMarkerIsAFinding) {
    const std::string article = changed("[A1]", "[a1]");
    const Report report = check_article(transcript(), ledger(), article);
    EXPECT_EQ(checks(report), (std::vector<Check>{Check::marker_valid, Check::atom_told}));
    EXPECT_EQ(report.findings[0].message, "[a1] is no marker: write it [A1]");
    ASSERT_TRUE(report.findings[0].article.has_value());
    EXPECT_EQ(text::text_of(article, *report.findings[0].article), "[a1]");
    EXPECT_EQ(report.coverage.markers, 4U);
    EXPECT_EQ(report.coverage.markers_invalid, 1U);
}

TEST(Article, AMarkerNamesAnAtomOfTheLedger) {
    const Report report = check_article(transcript(), ledger(), changed("[A4]", "[A4][A9]"));
    ASSERT_EQ(checks(report), std::vector<Check>{Check::marker_known});
    EXPECT_EQ(report.findings.front().message, "[A9] names no atom: the atoms are A1 to A4");
    EXPECT_EQ(report.coverage.markers_invalid, 1U);
}

TEST(Article, EveryAtomIsTold) {
    const Report report = check_article(transcript(), ledger(), changed(" [A4]", ""));
    ASSERT_EQ(checks(report), std::vector<Check>{Check::atom_told});
    EXPECT_EQ(report.findings.front().message, "A4 is not retold: tell it, and cite it [A4]");
    EXPECT_EQ(report.findings.front().sentences, std::vector<SentenceId>{SentenceId{5}}) << "what it retells";
    EXPECT_EQ(report.coverage.atoms_told, 3U);
    EXPECT_EQ(report.coverage.numbers, 2U) << "an atom not told is not held to its numbers twice";
}

TEST(Article, AnAtomsNumbersReachTheParagraphCitingIt) {
    const std::string article = changed("thirty-seven percent", "38 percent");
    const Report report = check_article(transcript(), ledger(), article);
    ASSERT_EQ(checks(report), std::vector<Check>{Check::number_kept});
    EXPECT_EQ(report.findings.front().message,
              "the paragraph citing A4 lacks \"37%\": write it, in digits or words");
    EXPECT_EQ(text::text_of(article, *report.findings.front().article),
              "Revenue grew 38 percent in twenty twenty-three [A4].");
    EXPECT_EQ(report.coverage.numbers_kept, 3U);
}

TEST(Article, ANameIsKeptWholeOrByItsLastWord) {
    EXPECT_TRUE(
        check_article(transcript(), ledger(), changed("Marie Curie won", "Curie won")).findings.empty());
    const Report report = check_article(transcript(), ledger(), changed("Marie Curie won", "She won"));
    ASSERT_EQ(checks(report), std::vector<Check>{Check::name_kept});
    EXPECT_EQ(report.findings.front().message, "the paragraph citing A2 does not name \"Marie Curie\"");
}

TEST(Article, ATermIsKept) {
    const Report report = check_article(transcript(), ledger(), changed("Nobel Prizes", "prizes"));
    ASSERT_EQ(checks(report), std::vector<Check>{Check::term_kept});
    EXPECT_EQ(report.findings.front().message,
              "the paragraph citing A2 does not use the term \"Nobel Prizes\"");
}

TEST(Article, AMustQuoteIsKeptVerbatim) {
    const Report report =
        check_article(transcript(), ledger(), changed("\"the most beautiful element\"", "a lovely element"));
    ASSERT_EQ(checks(report), std::vector<Check>{Check::must_quote_kept});
    EXPECT_EQ(
        report.findings.front().message,
        "A3 must be quoted verbatim, \"the most beautiful element\", and the paragraph citing it has nothing "
        "like it")
        << "one word of four in common is nothing to show";
    EXPECT_EQ(report.coverage.must_quotes_kept, 0U);
}

TEST(Article, EveryQuotationIsTheTranscriptsWords) {
    const Report report = check_article(transcript(), ledger(),
                                        changed("\"the most beautiful element\"",
                                                "\"the most beautiful element\", or \"the "
                                                "finest metal of all\","));
    ASSERT_EQ(checks(report), std::vector<Check>{Check::quote_verbatim});
    EXPECT_EQ(report.findings.front().message,
              "the quote \"the finest metal of all\" is nowhere in the transcript");
}

TEST(Article, ASoftenedQuotationShowsWhatWasSaid) {
    const Report report = check_article(transcript(), ledger(),
                                        changed("\"the most beautiful element\"",
                                                "\"the most beautiful element\" and "
                                                "\"radium the most lovely element\""));
    ASSERT_EQ(checks(report), std::vector<Check>{Check::quote_verbatim});
    EXPECT_EQ(
        report.findings.front().message,
        "the quote \"radium the most lovely element\" is not what was said: the transcript has \"radium the "
        "most beautiful element\"");
}

TEST(Article, AnEllipsisLeavesWordsOut) {
    EXPECT_TRUE(check_article(transcript(), ledger(),
                              changed("\"the most beautiful element\"",
                                      "\"the most beautiful element\" (\"radium the "
                                      "most \u2026 element\")"))
                    .findings.empty());
}

TEST(Article, AShortQuotationIsANameOrATerm) {
    EXPECT_TRUE(check_article(transcript(), ledger(), changed("Water boils", "\"Pure\" water boils"))
                    .findings.empty());
}

TEST(Article, WithinASectionTheTalksOrderIsKept) {
    const std::string article =
        "# All\n\n"
        "Revenue grew 37% in 2023 [A4]. Water boils at 100 degrees at sea level [A1]. Marie Curie won two "
        "Nobel Prizes [A2]. She called radium \"the most beautiful element\" [A3].\n";
    const Report report = check_article(transcript(), ledger(), article);
    ASSERT_EQ(checks(report), std::vector<Check>{Check::order_kept})
        << "only A4 is out of place, not the rest";
    EXPECT_EQ(report.findings.front().severity, Severity::warning);
    EXPECT_EQ(report.findings.front().atoms, std::vector<AtomId>{AtomId{4}});
    EXPECT_EQ(report.findings.front().message,
              "A4 was said at S5, before what this section tells ahead of it: keep the order of the talk");
}

TEST(Article, AnAtomToldAgainIsPlacedWhereFirstTold) {
    const std::string article = changed("\"the most beautiful element\" [A3].",
                                        "\"the most beautiful element\" [A3]. Water, recall, boils at 100 "
                                        "degrees [A1].");
    EXPECT_TRUE(check_article(transcript(), ledger(), article).findings.empty())
        << "a recap tells nothing out of order";
}

TEST(Article, AtomsSaidTogetherMayBeToldInEitherOrder) {
    AtomLedger together = ledger();
    together.atoms[2].sentences = {SentenceId{3}, SentenceId{4}};  // "Marie Curie" of A3 is said in S3
    const std::string article = changed(
        "Marie Curie won two Nobel Prizes [A2]. She called radium \"the most beautiful element\" [A3].",
        "Marie Curie called radium \"the most beautiful element\" [A3]. She won two Nobel Prizes [A2].");
    EXPECT_TRUE(check_article(transcript(), together, article).findings.empty())
        << "A2 and A3 were both first said at S3";
}

TEST(Article, SectionsMayBeOrderedFreely) {
    const std::string article =
        "# Money\n\n"
        "Revenue grew 37% in 2023 [A4].\n\n"
        "# Science\n\n"
        "Water boils at 100 degrees at sea level [A1]. Marie Curie won two Nobel Prizes [A2]. "
        "She called radium \"the most beautiful element\" [A3].\n";
    EXPECT_TRUE(check_article(transcript(), ledger(), article).findings.empty());
}

TEST(Article, CodeCitesNothing) {
    const std::string article = changed("Revenue grew thirty-seven percent in twenty twenty-three [A4].",
                                        "```\nrevenue = 37  # [A4]\n```");
    EXPECT_EQ(checks(check_article(transcript(), ledger(), article)), std::vector<Check>{Check::atom_told});
}

TEST(Article, QuotesInCodeAreCode) {
    // The block is fenced with tildes: fenced with backticks, it would read as
    // inline code as well, and the test would hold one rule twice.
    const std::string article = changed("[A4].",
                                        "[A4]. The ledger logs it as `note(\"the finest metal of all\")`.\n\n"
                                        "~~~\nprint(\"the finest metal of all\")\n~~~");
    EXPECT_TRUE(check_article(transcript(), ledger(), article).findings.empty())
        << "in inline code and in a code block alike";
}

TEST(Article, AnInchMarkIsNoQuote) {
    EXPECT_TRUE(
        check_article(transcript(), ledger(),
                      changed("[A4].", "[A4]. The talk was shown on a 55\" screen and a 13\" laptop."))
            .findings.empty());
}

TEST(Article, CodeInAQuotationIsReadAsItsWords) {
    const std::string article = changed("\"the most beautiful element\"",
                                        R"("the most beautiful element", "the most `beautiful` metal")");
    EXPECT_EQ(checks(check_article(transcript(), ledger(), article)),
              std::vector<Check>{Check::quote_verbatim});
}

// --- What a text is held to ------------------------------------------------------------

TEST(Article, AScopeHoldsATextToItsOwnAtoms) {
    const std::vector<AtomId> first_two{AtomId{1}, AtomId{2}};
    const std::string section =
        "Water boils at 100 degrees at sea level [A1]. Marie Curie won two Nobel Prizes [A2].\n";
    const Report report = check_article(transcript(), ledger(), section, {.told = first_two});
    EXPECT_TRUE(report.findings.empty()) << report.findings.front().message;
    EXPECT_EQ(report.coverage.atoms, 2U) << "A3 and A4 are another text's to tell";
    EXPECT_EQ(report.coverage.atoms_told, 2U);

    const Report wide = check_article(transcript(), ledger(), kArticle, {.told = first_two});
    ASSERT_EQ(checks(wide), (std::vector<Check>{Check::marker_scope, Check::marker_scope}));
    EXPECT_EQ(wide.findings.front().message,
              "[A3] is not among the atoms to tell here: cite only the atoms given");
    EXPECT_EQ(wide.coverage.markers_invalid, 0U) << "it names an atom all the same";
}

TEST(Article, ADuplicateIsToldByTheAtomItRepeats) {
    const std::vector<knowledge::Duplicate> repeats{knowledge::Duplicate{.atom = AtomId{4}, .of = AtomId{1}}};
    const std::string without_a4 =
        changed("Revenue grew thirty-seven percent in twenty twenty-three [A4].\n", "");
    const Report report = check_article(transcript(), ledger(), without_a4, {.duplicates = repeats});
    EXPECT_TRUE(report.findings.empty()) << report.findings.front().message;
    EXPECT_EQ(report.coverage.atoms, 4U);
    EXPECT_EQ(report.coverage.atoms_told, 4U) << "A4, by A1";

    const std::string without_a1 =
        replace_all(without_a4, "Water boils at 100 degrees at sea level [A1]. ", "");
    const Report untold = check_article(transcript(), ledger(), without_a1, {.duplicates = repeats});
    ASSERT_EQ(checks(untold), std::vector<Check>{Check::atom_told});
    EXPECT_EQ(untold.findings.front().atoms, std::vector<AtomId>{AtomId{1}})
        << "said once, of the atom to tell";
    EXPECT_EQ(untold.coverage.atoms_told, 2U) << "neither A1 nor the A4 it would tell";
}

}  // namespace
}  // namespace rr::fidelity

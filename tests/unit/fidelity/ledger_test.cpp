#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <rr/fidelity/checks.hpp>

#include "fixture.hpp"

namespace rr::fidelity {
namespace {

using fixture::AtomId;
using fixture::AtomLedger;
using fixture::checks;
using fixture::Filler;
using fixture::FillerLabel;
using fixture::ledger;
using fixture::SentenceId;
using fixture::Transcript;
using fixture::transcript;

TEST(Ledger, AFaithfulLedgerHasNoFinding) {
    const Report report = check_ledger(transcript(), ledger());
    EXPECT_TRUE(report.findings.empty()) << report.findings.front().message;
    EXPECT_EQ(report.coverage.sentences, 6U);
    EXPECT_EQ(report.coverage.sentences_accounted, 6U);
}

TEST(Ledger, EverySentenceIsAccountedFor) {
    AtomLedger missing = ledger();
    missing.fillers.pop_back();
    const Report report = check_ledger(transcript(), missing);
    ASSERT_EQ(checks(report), std::vector<Check>{Check::sentence_accounted});
    const Finding& finding = report.findings.front();
    EXPECT_EQ(finding.severity, Severity::error);
    EXPECT_EQ(finding.sentences, std::vector<SentenceId>{SentenceId{6}});
    EXPECT_EQ(finding.message,
              "S6 is not accounted for: give it one or more atoms, or a filler label (disfluency, greeting, "
              "repetition, sponsor, decorative)");
    EXPECT_EQ(report.coverage.sentences_accounted, 5U);
}

TEST(Ledger, ASentenceIsAtomsOrFillerNotBoth) {
    AtomLedger both = ledger();
    both.fillers.insert(both.fillers.begin() + 1,
                        Filler{.sentence = SentenceId{2}, .label = FillerLabel::repetition});
    const Report report = check_ledger(transcript(), both);
    ASSERT_EQ(checks(report), std::vector<Check>{Check::sentence_once});
    EXPECT_EQ(report.findings.front().message,
              "S2 is labelled repetition and told by A1: it is one or the other");
    EXPECT_EQ(report.findings.front().atoms, std::vector<AtomId>{AtomId{1}});
    EXPECT_EQ(report.coverage.sentences_accounted, 5U);
}

TEST(Ledger, OnlyTheSentencesGivenAreNamed) {
    AtomLedger stray = ledger();
    stray.atoms[0].sentences.emplace_back(9U);
    stray.fillers.emplace_back(SentenceId{8}, FillerLabel::sponsor);
    const Report report = check_ledger(transcript(), stray);
    ASSERT_EQ(checks(report), (std::vector<Check>{Check::sentence_known, Check::sentence_known}));
    EXPECT_EQ(report.findings[0].message,
              "A1 cites S9, which the transcript given has not: cite only the sentences you were given");
    EXPECT_EQ(report.findings[1].message, "a filler labels S8, which the transcript given has not");
}

TEST(Ledger, AnAtomSaysOnlyTheNumbersItsSentencesSay) {
    AtomLedger wrong = ledger();
    wrong.atoms[3].text = "Revenue grew 38% in 2023.";
    const Report report = check_ledger(transcript(), wrong);
    ASSERT_EQ(checks(report), std::vector<Check>{Check::number_said});
    EXPECT_EQ(report.findings.front().message,
              "A4 says \"38%\", which its sentences (S5) do not: they say \"37%\", \"2023\"");
    EXPECT_EQ(report.findings.front().sentences, std::vector<SentenceId>{SentenceId{5}});
}

TEST(Ledger, NumbersAreComparedByValue) {
    AtomLedger spoken = ledger();
    spoken.atoms[3].text = "Revenue grew thirty-seven percent in twenty twenty-three.";
    EXPECT_TRUE(check_ledger(transcript(), spoken).findings.empty());
    spoken.atoms[3].text = "Revenue grew 37 in 2023.";
    EXPECT_EQ(checks(check_ledger(transcript(), spoken)), std::vector<Check>{Check::number_said})
        << "37 is not 37%";
}

TEST(Ledger, AMustQuoteIsWhatWasSaid) {
    AtomLedger softened = ledger();
    softened.atoms[2].quote = "the most wonderful element";
    Report report = check_ledger(transcript(), softened);
    ASSERT_EQ(checks(report), std::vector<Check>{Check::quote_said});
    EXPECT_EQ(report.findings.front().message,
              "A3's quote \"the most wonderful element\" is not what was said: its sentences say \"the most "
              "beautiful element\"");
    softened.atoms[2].quote = "entirely different words";
    report = check_ledger(transcript(), softened);
    ASSERT_EQ(checks(report), std::vector<Check>{Check::quote_said});
    EXPECT_EQ(report.findings.front().message,
              "A3's quote \"entirely different words\" is nowhere in its sentences (S4)");
}

TEST(Ledger, ANameTheTranscriptNeverGivesIsAWarning) {
    AtomLedger invented = ledger();
    invented.atoms[1].names.emplace_back("Albert Einstein");
    const Report report = check_ledger(transcript(), invented);
    ASSERT_EQ(checks(report), std::vector<Check>{Check::name_said});
    EXPECT_EQ(report.findings.front().severity, Severity::warning);
    EXPECT_EQ(report.errors(), 0U);
}

TEST(Ledger, AFillerHoldsNoNumberNameOrTerm) {
    Transcript holding = transcript();
    holding.sentences[5].text = "Um, 42, you know.";
    Report report = check_ledger(holding, ledger());
    ASSERT_EQ(checks(report), std::vector<Check>{Check::filler_empty});
    EXPECT_EQ(
        report.findings.front().message,
        "S6 is labelled disfluency, yet holds the number \"42\": re-examine it, and give it atoms unless it "
        "carries nothing");
    holding.sentences[5].text = "Um, ask Ada Lovelace, you know.";
    report = check_ledger(holding, ledger());
    ASSERT_EQ(checks(report), std::vector<Check>{Check::filler_empty});
    EXPECT_NE(report.findings.front().message.find("the name \"Ada Lovelace\""), std::string::npos);
    holding.sentences[5].text = "Um, radium, you know.";
    report = check_ledger(holding, ledger());
    ASSERT_EQ(checks(report), std::vector<Check>{Check::filler_empty});
    EXPECT_NE(report.findings.front().message.find("the term \"radium\""), std::string::npos)
        << "a lexicon term, from the atoms' terms";
    holding.sentences[5].text = "Um, polonium, you know.";
    EXPECT_TRUE(check_ledger(holding, ledger()).findings.empty()) << "no atom uses the term";
    const std::vector<std::string> lexicon{"polonium"};
    report = check_ledger(holding, ledger(), lexicon);
    ASSERT_EQ(checks(report), std::vector<Check>{Check::filler_empty});
    EXPECT_NE(report.findings.front().message.find("the term \"polonium\""), std::string::npos)
        << "a term of the video's lexicon, which no atom of the window uses";
}

TEST(Ledger, ABareOneIsNotHeld) {
    Transcript aside = transcript();
    aside.sentences[5].text = "Um, this is a fun one, you know.";
    EXPECT_TRUE(check_ledger(aside, ledger()).findings.empty())
        << "a spoken \"one\" is as often a pronoun as a count";
    aside.sentences[5].text = "Um, one percent, you know.";
    EXPECT_EQ(checks(check_ledger(aside, ledger())), std::vector<Check>{Check::filler_empty})
        << "\"one percent\" is a number";
}

TEST(Ledger, AWindowIsCheckedAlone) {
    const Transcript all = transcript();
    const Transcript window{.sentences = {all.sentences[1], all.sentences[2]}};
    const AtomLedger atoms{.atoms = {ledger().atoms[0], ledger().atoms[1]}};
    const Report report = check_ledger(window, atoms);
    EXPECT_TRUE(report.findings.empty());
    EXPECT_EQ(report.coverage.sentences, 2U);
}

}  // namespace
}  // namespace rr::fidelity

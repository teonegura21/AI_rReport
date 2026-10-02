// Whether an atom may be declared a duplicate of another: only when telling
// the other loses nothing it says, nor how it says it.
#include <optional>
#include <string>
#include <utility>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/fidelity/checks.hpp>
#include <rr/knowledge/atoms.hpp>

namespace rr::fidelity {
namespace {

using knowledge::Atom;
using knowledge::AtomId;
using knowledge::Modality;
using knowledge::SentenceId;
using ::testing::ElementsAre;
using ::testing::IsEmpty;

// A12, said by Ada: the atom another may repeat.
Atom told() {
    return Atom{.id = AtomId{12},
                .text = "Marie Curie weighed thirty-seven grams of radium in 1902.",
                .sentences = {SentenceId{40}},
                .speaker = "Ada",
                .names = {"Marie Curie"},
                .terms = {"radium"}};
}

// A15, saying A12 again: by default, a duplicate it may be declared.
Atom again() {
    return Atom{.id = AtomId{15},
                .text = "In 1902, Curie had 37 grams of radium.",
                .sentences = {SentenceId{44}},
                .speaker = "ada",
                .names = {"Curie"},
                .terms = {"Radium"}};
}

TEST(Duplicates, MayBeDeclaredWhenTellingTheOtherLosesNothing) {
    EXPECT_THAT(duplicate_problems(again(), told()), IsEmpty())
        << "numbers by value, a name by its last word, words and speakers folded";
}

TEST(Duplicates, AreNotWhereTheAtomSaysMore) {
    Atom more = again();
    more.text = "In 1902, Curie had 37 grams of radium, worth 100,000 dollars.";
    EXPECT_THAT(duplicate_problems(more, told()), ElementsAre("it says \"100,000\", which A12 does not"));

    Atom named = again();
    named.names.emplace_back("Pierre");
    EXPECT_THAT(duplicate_problems(named, told()), ElementsAre("it names \"Pierre\", which A12 does not"));

    Atom termed = again();
    termed.terms.emplace_back("half-life");
    EXPECT_THAT(duplicate_problems(termed, told()), ElementsAre("it uses \"half-life\", which A12 does not"));

    Atom listed = told();
    listed.id = AtomId{20};
    listed.text = "In 1902 she had thirty-seven grams.";
    EXPECT_THAT(duplicate_problems(again(), listed), IsEmpty()) << "what the other lists, telling it gives";
}

TEST(Duplicates, AreNotWhereTheAtomsWordsAreToBeQuoted) {
    Atom quoting = again();
    quoting.must_quote = true;
    quoting.quote = "the most precious thing";
    EXPECT_THAT(
        duplicate_problems(quoting, told()),
        ElementsAre("its words \"the most precious thing\" are to be quoted, and A12 does not quote them"));

    Atom quoted = told();
    quoted.must_quote = true;
    quoted.quote = "radium, the most precious thing in the shed";
    EXPECT_THAT(duplicate_problems(quoting, quoted), IsEmpty()) << "the other quotes them, word for word";
    quoted.quote = "the most valuable thing";
    EXPECT_THAT(duplicate_problems(quoting, quoted), ::testing::SizeIs(1));
}

TEST(Duplicates, AreNotWhereTheAtomIsSaidOtherwise) {
    Atom hedged = again();
    hedged.modality = Modality::hedged;
    EXPECT_THAT(duplicate_problems(hedged, told()), ElementsAre("it is hedged, and A12 asserted"));
    Atom asked = again();
    asked.modality = Modality::question;
    EXPECT_THAT(duplicate_problems(asked, told()), ElementsAre("it is a question, and A12 asserted"));

    Atom pierre = again();
    pierre.speaker = "Pierre";
    EXPECT_THAT(duplicate_problems(pierre, told()), ElementsAre("it is said by Pierre, and A12 by Ada"));
    Atom unknown = again();
    unknown.speaker.reset();
    EXPECT_THAT(duplicate_problems(unknown, told()),
                ElementsAre("it is said by a speaker the transcript does not name, and A12 by Ada"));

    Atom reporting = again();
    reporting.attributed_to = "the NRC";
    EXPECT_THAT(duplicate_problems(reporting, told()),
                ElementsAre("it reports the NRC, and A12 reports no one"));
    EXPECT_THAT(duplicate_problems(again(), reporting),
                ElementsAre("it reports no one, and A15 reports the NRC"));
}

}  // namespace
}  // namespace rr::fidelity

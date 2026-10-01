// A small talk, its faithful Ledger A, and the article that retells it
// faithfully: each test breaks one promise, and expects exactly the finding
// for it.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <rr/core/time_span.hpp>
#include <rr/fidelity/checks.hpp>
#include <rr/knowledge/atoms.hpp>
#include <rr/knowledge/transcript.hpp>
#include <rr/testkit/result.hpp>

namespace rr::fidelity::fixture {

using knowledge::Atom;
using knowledge::AtomId;
using knowledge::AtomKind;
using knowledge::AtomLedger;
using knowledge::Filler;
using knowledge::FillerLabel;
using knowledge::Sentence;
using knowledge::SentenceId;
using knowledge::Transcript;

inline Sentence sentence(std::uint32_t number, std::string text) {
    const auto start = Millis{static_cast<std::int64_t>(number) * 1000};
    return Sentence{.id = SentenceId{number},
                    .text = std::move(text),
                    .span = testkit::unwrap(TimeSpan::make(start, start + Millis{900})),
                    .speaker = "Ada"};
}

inline Transcript transcript() {
    return Transcript{.sentences = {
                          sentence(1, "Hello everyone, welcome back."),
                          sentence(2, "Water boils at 100 degrees at sea level."),
                          sentence(3, "Marie Curie won two Nobel Prizes."),
                          sentence(4, "She called radium the most beautiful element."),
                          sentence(5, "Revenue grew 37% in 2023."),
                          sentence(6, "Um, you know."),
                      }};
}

inline AtomLedger ledger() {
    return AtomLedger{
        .atoms =
            {
                Atom{.id = AtomId{1},
                     .kind = AtomKind::fact,
                     .text = "Water boils at 100 degrees at sea level.",
                     .sentences = {SentenceId{2}}},
                Atom{.id = AtomId{2},
                     .kind = AtomKind::fact,
                     .text = "Marie Curie won two Nobel Prizes.",
                     .sentences = {SentenceId{3}},
                     .names = {"Marie Curie"},
                     .terms = {"Nobel Prizes"}},
                Atom{.id = AtomId{3},
                     .kind = AtomKind::opinion,
                     .text = "Marie Curie called radium the most beautiful element.",
                     .sentences = {SentenceId{4}},
                     .must_quote = true,
                     .quote = "the most beautiful element",
                     .terms = {"radium"}},
                Atom{.id = AtomId{4},
                     .kind = AtomKind::number,
                     .text = "Revenue grew 37% in 2023.",
                     .sentences = {SentenceId{5}}},
            },
        .fillers = {Filler{.sentence = SentenceId{1}, .label = FillerLabel::greeting},
                    Filler{.sentence = SentenceId{6}, .label = FillerLabel::disfluency}},
    };
}

inline const std::string kArticle =
    "# Science and money\n"
    "\n"
    "Water boils at 100 degrees at sea level [A1]. Marie Curie won two Nobel Prizes [A2]. "
    "She called radium \"the most beautiful element\" [A3].\n"
    "\n"
    "Revenue grew thirty-seven percent in twenty twenty-three [A4].\n";

// The checks that fired, in order.
inline std::vector<Check> checks(const Report& report) {
    std::vector<Check> found;
    found.reserve(report.findings.size());
    for (const Finding& finding : report.findings) {
        found.push_back(finding.check);
    }
    return found;
}

}  // namespace rr::fidelity::fixture

// The rule every fault is tested against (docs/design/testing.md): it ends
// repaired, or published as a finding, never silently in the article. Here,
// the oracle's faults that change what a model writes are made on a faithful
// answer, and a check must catch each.
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <rr/core/strings.hpp>
#include <rr/json/json.hpp>
#include <rr/oracle/oracle.hpp>

#include "fixture.hpp"

namespace rr::fidelity {
namespace {

using fixture::AtomLedger;
using fixture::checks;
using fixture::kArticle;
using fixture::ledger;
using fixture::transcript;

// `answer`, as the model would have written it with `fault`.
std::string with_fault(const oracle::Fault& fault, std::string answer) {
    llm::Choice choice{.content = std::move(answer)};
    EXPECT_TRUE(fault.change(choice)) << fault.name << " found something to change";
    return choice.content;
}

TEST(Faults, AWrongNumberInTheArticleIsCaught) {
    const std::string article = with_fault(oracle::wrong_number({}), kArticle);
    ASSERT_NE(article.find("101 degrees"), std::string::npos);
    EXPECT_EQ(checks(check_article(transcript(), ledger(), article)), std::vector<Check>{Check::number_kept});
}

TEST(Faults, AWrongNumberInAnAtomIsCaught) {
    AtomLedger atoms = ledger();
    atoms.atoms[3].text = with_fault(oracle::wrong_number({}), atoms.atoms[3].text);
    EXPECT_EQ(checks(check_ledger(transcript(), atoms)), std::vector<Check>{Check::number_said});
}

TEST(Faults, AMarkerCitingNoAtomIsCaught) {
    const std::string article = with_fault(oracle::bad_marker({}), kArticle);
    EXPECT_EQ(checks(check_article(transcript(), ledger(), article)),
              (std::vector<Check>{Check::marker_known, Check::atom_told}))
        << "the marker names no atom, and the atom it replaced goes untold";
}

TEST(Faults, ADroppedSentenceIsRefusedOrCaught) {
    const std::string answer =
        with_fault(oracle::dropped_reference({}), json::dump(knowledge::to_json(ledger())));
    const Result<json::Json> document = json::parse(answer);
    ASSERT_TRUE(document.has_value());
    const Result<AtomLedger> read = knowledge::atom_ledger_from_json(*document);
    if (!read.has_value()) {
        SUCCEED() << "the closed world refuses it as it is read: " << read.error().message();
        return;
    }
    const std::vector<Check> found = checks(check_ledger(transcript(), *read));
    EXPECT_NE(std::ranges::find(found, Check::sentence_accounted), found.end());
}

TEST(Faults, ASoftenedQuoteIsCaught) {
    const oracle::Fault soften = oracle::edit("soften", {}, [](const std::string& article) {
        return replace_all(article, "the most beautiful element", "a rather nice element");
    });
    const std::string article = with_fault(soften, kArticle);
    EXPECT_EQ(checks(check_article(transcript(), ledger(), article)),
              (std::vector<Check>{Check::must_quote_kept, Check::quote_verbatim}));
}

TEST(Faults, AMisattributedNameIsCaught) {
    const oracle::Fault misattribute = oracle::edit("misattribute", {}, [](const std::string& article) {
        return replace_all(article, "Marie Curie won", "Lise Meitner won");
    });
    const std::string article = with_fault(misattribute, kArticle);
    EXPECT_EQ(checks(check_article(transcript(), ledger(), article)), std::vector<Check>{Check::name_kept});
}

}  // namespace
}  // namespace rr::fidelity

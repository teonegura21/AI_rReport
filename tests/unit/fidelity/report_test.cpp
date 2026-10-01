#include <set>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <rr/fidelity/finding.hpp>

namespace rr::fidelity {
namespace {

TEST(Report, ChecksHaveStableNames) {
    EXPECT_EQ(to_string(Check::sentence_accounted), "sentence-accounted");
    EXPECT_EQ(to_string(Check::must_quote_kept), "must-quote-kept");
    EXPECT_EQ(to_string(Check::order_kept), "order-kept");
    std::set<std::string_view> names;
    for (int check = 0; check <= static_cast<int>(Check::order_kept); ++check) {
        names.insert(to_string(static_cast<Check>(check)));
    }
    EXPECT_EQ(names.size(), static_cast<std::size_t>(Check::order_kept) + 1) << "one name each";
}

TEST(Report, CountsItsErrors) {
    const Report report{.findings = {Finding{.check = Check::atom_told},
                                     Finding{.check = Check::order_kept, .severity = Severity::warning},
                                     Finding{.check = Check::number_kept}}};
    EXPECT_EQ(report.errors(), 2U);
}

TEST(Report, ReportsAddUp) {
    Report ledger{.findings = {Finding{.check = Check::sentence_accounted}},
                  .coverage = Coverage{.sentences = 10, .sentences_accounted = 9}};
    const Report article{.findings = {Finding{.check = Check::atom_told}},
                         .coverage = Coverage{.atoms = 5,
                                              .atoms_told = 4,
                                              .markers = 7,
                                              .markers_invalid = 1,
                                              .must_quotes = 2,
                                              .must_quotes_kept = 2,
                                              .numbers = 3,
                                              .numbers_kept = 3}};
    ledger.add(article);
    EXPECT_EQ(ledger.findings.size(), 2U);
    EXPECT_EQ(ledger.findings[1].check, Check::atom_told);
    EXPECT_EQ(ledger.coverage, (Coverage{.sentences = 10,
                                         .sentences_accounted = 9,
                                         .atoms = 5,
                                         .atoms_told = 4,
                                         .markers = 7,
                                         .markers_invalid = 1,
                                         .must_quotes = 2,
                                         .must_quotes_kept = 2,
                                         .numbers = 3,
                                         .numbers_kept = 3}));
}

}  // namespace
}  // namespace rr::fidelity

#include <rr/knowledge/ids.hpp>

#include <format>
#include <optional>
#include <string_view>
#include <unordered_set>

#include <gtest/gtest.h>

namespace rr::knowledge {
namespace {

TEST(ShortIds, AreWrittenAsTheirLetterAndNumber) {
    EXPECT_EQ(SentenceId{12}.str(), "S12");
    EXPECT_EQ(AtomId{41}.str(), "A41");
    EXPECT_EQ(VisualId{3}.str(), "V3");
    EXPECT_EQ(std::format("cites {}", AtomId{7}), "cites A7");
}

TEST(ShortIds, ParseOnlyTheirExactForm) {
    EXPECT_EQ(AtomId::parse("A41"), AtomId{41});
    EXPECT_EQ(AtomId::parse("A999999999"), AtomId{999'999'999});
    for (const std::string_view loose :
         {"", "A", "A0", "A041", "a41", "A 41", " A41", "A41 ", "A4x", "S41", "A-1", "A1234567890", "41"}) {
        EXPECT_EQ(AtomId::parse(loose), std::nullopt) << loose;
    }
    EXPECT_EQ(SentenceId::parse("S12"), SentenceId{12});
    EXPECT_EQ(SentenceId::parse("A12"), std::nullopt) << "each kind of ID has its own letter";
}

TEST(ShortIds, AreOrderedByNumber) {
    EXPECT_LT(SentenceId{2}, SentenceId{10});
    EXPECT_EQ(SentenceId{5}, SentenceId{5});
}

TEST(ShortIds, CanBeHashed) {
    const std::unordered_set<AtomId> atoms{AtomId{1}, AtomId{2}, AtomId{1}};
    EXPECT_EQ(atoms.size(), 2U);
}

TEST(ShortIdsDeathTest, AreNumberedFromOne) {
    EXPECT_DEATH((void)AtomId{0}, "numbered from 1");
    EXPECT_DEATH((void)AtomId{1'000'000'000}, "numbered from 1");
}

}  // namespace
}  // namespace rr::knowledge

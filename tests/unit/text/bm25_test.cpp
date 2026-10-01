#include <rr/text/bm25.hpp>

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

namespace rr::text {
namespace {

std::vector<std::size_t> passages_of(const std::vector<Hit>& hits) {
    std::vector<std::size_t> found;
    found.reserve(hits.size());
    for (const Hit& hit : hits) {
        found.push_back(hit.passage);
    }
    return found;
}

TEST(Bm25, PassagesAreNumberedInOrder) {
    Bm25 index;
    EXPECT_EQ(index.add("first"), 0U);
    EXPECT_EQ(index.add("second"), 1U);
    EXPECT_EQ(index.size(), 2U);
}

TEST(Bm25, ScoresFollowTheFormula) {
    Bm25 index;
    index.add("the cat sat");                // 3 terms
    index.add("the dog barked at the cat");  // 6 terms
    index.add("a bird");                     // 2 terms
    const std::vector<Hit> hits = index.search("dog", 10);
    ASSERT_EQ(hits.size(), 1U);
    // N = 3 passages, n = 1 holds "dog"; f = 1; length 6, average 11 / 3.
    const double idf = std::log(1.0 + ((3.0 - 1.0 + 0.5) / (1.0 + 0.5)));
    const double expected = idf * 1.0 * 2.2 / (1.0 + (1.2 * (1.0 - 0.75 + (0.75 * 6.0 / (11.0 / 3.0)))));
    EXPECT_EQ(hits[0].passage, 1U);
    EXPECT_NEAR(hits[0].score, expected, 1e-12);
}

TEST(Bm25, ARareTermWeighsMoreThanACommonOne) {
    Bm25 index;
    index.add("the market fell");
    index.add("the market rose");
    index.add("the crash came");
    const std::vector<Hit> hits = index.search("market crash", 10);
    ASSERT_EQ(hits.size(), 3U);
    EXPECT_EQ(hits[0].passage, 2U) << R"("crash" is in one passage, "market" in two)";
}

TEST(Bm25, RepetitionsSaturate) {
    Bm25 index({.k1 = 1.2, .b = 0.0});
    index.add("fact");
    index.add("fact fact fact fact fact fact fact fact");
    index.add("other");
    const std::vector<Hit> hits = index.search("fact", 10);
    ASSERT_EQ(hits.size(), 2U);
    EXPECT_EQ(hits[0].passage, 1U);
    EXPECT_LT(hits[0].score, 2.2 * hits[1].score)
        << "eight times the count, never more than k1 + 1 times the score of one";
}

TEST(Bm25, ALongerPassageWeighsLessWithB) {
    Bm25 index;
    index.add("fact and many more words besides");
    index.add("fact");
    EXPECT_EQ(passages_of(index.search("fact", 10)), (std::vector<std::size_t>{1, 0}));

    Bm25 flat({.k1 = 1.2, .b = 0.0});
    flat.add("fact");
    flat.add("fact and many more words besides");
    const std::vector<Hit> hits = flat.search("fact", 10);
    ASSERT_EQ(hits.size(), 2U);
    EXPECT_DOUBLE_EQ(hits[0].score, hits[1].score) << "b = 0: length does not count";
}

TEST(Bm25, TiesGoToTheEarlierPassage) {
    Bm25 index;
    index.add("same words");
    index.add("unrelated");
    index.add("same words");
    EXPECT_EQ(passages_of(index.search("same", 10)), (std::vector<std::size_t>{0, 2}));
}

TEST(Bm25, TopLimitsTheHits) {
    Bm25 index;
    for (int passage = 0; passage < 5; ++passage) {
        index.add("word");
    }
    EXPECT_EQ(index.search("word", 2).size(), 2U);
    EXPECT_TRUE(index.search("word", 0).empty());
}

TEST(Bm25, PassagesWithoutAQueryTermAreLeftOut) {
    Bm25 index;
    index.add("alpha");
    index.add("beta");
    EXPECT_EQ(passages_of(index.search("alpha gamma", 10)), (std::vector<std::size_t>{0}));
    EXPECT_TRUE(index.search("gamma", 10).empty());
}

TEST(Bm25, AQueryTermCountsOnce) {
    Bm25 index;
    index.add("cat");
    index.add("dog");
    EXPECT_EQ(index.search("cat cat cat", 10), index.search("cat", 10));
}

TEST(Bm25, TermsAreComparedFolded) {
    Bm25 index;
    index.add("Caf\u00E9 society");
    EXPECT_EQ(passages_of(index.search("CAFE", 10)), (std::vector<std::size_t>{0}));
}

TEST(Bm25, AnEmptyIndexFindsNothing) {
    const Bm25 empty;
    EXPECT_TRUE(empty.search("anything", 10).empty());
    Bm25 blank;
    blank.add("");
    blank.add("!!");
    EXPECT_TRUE(blank.search("anything", 10).empty());
}

TEST(Bm25DeathTest, ParametersOutOfRangeAreRefused) {
    EXPECT_DEATH((void)Bm25({.k1 = -1.0}), "BM25");
    EXPECT_DEATH((void)Bm25({.b = 1.5}), "BM25");
}

}  // namespace
}  // namespace rr::text

// A person's verdicts on what the eval's judges read: the file written and
// read back strictly, the verdicts as sides, and the draws.
#include <rr/eval/labels.hpp>

#include <algorithm>
#include <format>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

namespace rr::eval {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using testkit::unwrap;

Labels labels() {
    return Labels{.name = "r5",
                  .panel = "eval-v1",
                  .run = "r4-max",
                  .against = "r4-fast",
                  .labeller = "Ada",
                  .facts = {FactLabel{.job = "J1",
                                      .video = "lecture",
                                      .replicate = 0,
                                      .fact = "F3",
                                      .core = true,
                                      .said = "1:02 to 1:09",
                                      .claim = "Radium glows in the dark.",
                                      .quote = "it \"glows\", she said",
                                      .shown = {"1. Radium glows.", "2. It is warm."},
                                      .verdict = FactVerdict::partly}},
                  .sentences = {SentenceLabel{.job = "J1",
                                              .video = "lecture",
                                              .replicate = 1,
                                              .number = 14,
                                              .text = "It glows\nfaintly.",
                                              .evidence = {"S12 [1:02] It glows."}}},
                  .parts = {PartLabel{.job = "P1",
                                      .video = "lecture",
                                      .replicate = 0,
                                      .part = 2,
                                      .stretch = "1:30 to 3:55",
                                      .b_first = true,
                                      .first = "## Why\n\nBecause.",
                                      .second = "## Why it glows\n\nBecause it is radioactive.",
                                      .verdict = reason::Preference::second}}};
}

TEST(Labels, AreWrittenForAPersonAndReadBackAsTheyWere) {
    const Labels written = labels();
    const std::string text = to_toml(written);
    EXPECT_THAT(text, HasSubstr("# Labels that calibrate the panel eval-v1")) << "it says what to do";
    EXPECT_THAT(text, HasSubstr("rreport eval calibrate r5"));
    EXPECT_THAT(text, HasSubstr("verdict = \"partly\"  # told | partly | not_told"));
    EXPECT_THAT(text, HasSubstr("verdict = \"\"  # none | adds | softens | intensifies"))
        << "a verdict not given yet is empty";
    EXPECT_THAT(text, HasSubstr("order = \"ba\""));
    EXPECT_EQ(unwrap(parse_labels("r5", text)), written) << "every field, quotes and line breaks too";
}

TEST(Labels, AreReadFromTheirFileByAName) {
    const testkit::TempDir dir;
    (void)dir.write_file("labels/r5.toml", to_toml(labels()));
    EXPECT_EQ(unwrap(read_labels(dir.path(), "r5")), labels());
    EXPECT_EQ(labels_file(dir.path(), "r5"), dir.path() / "labels" / "r5.toml");
    EXPECT_ERRC(read_labels(dir.path(), "r6"), Errc::not_found);
    EXPECT_ERRC(read_labels(dir.path(), "../r5"), Errc::invalid_argument) << "a name, never a path";
}

TEST(Labels, EveryMistakeIsRefusedNamingItAndWhereItIs) {
    const std::string good = to_toml(labels());
    const auto refused = [&](std::string_view from, std::string_view to, std::string_view said) {
        std::string text = good;
        const std::size_t at = text.find(from);
        ASSERT_NE(at, std::string::npos) << from;
        text.replace(at, from.size(), to);
        const Result<Labels> parsed = parse_labels("r5", text);
        ASSERT_FALSE(parsed.has_value()) << to;
        EXPECT_EQ(parsed.error().code(), Errc::invalid_argument) << to;
        EXPECT_THAT(parsed.error().message(), HasSubstr(said)) << to;
    };
    refused("name = \"r5\"", "name = \"r6\"", R"('name' must be the file's, "r5", not "r6")");
    refused("verdict = \"partly\"", "verdict = \"half\"",
            R"(fact 1: 'verdict' must be empty or one of: told, partly, not_told, not "half")");
    refused("verdict = \"second\"", "verdict = \"b\"", "part 1: 'verdict' must be empty or one of: first");
    refused("order = \"ba\"", "order = \"b\"", "part 1: 'order' must be one of: ab, ba");
    refused("number = 14", "number = 0", "sentence 1: 'number' must be a whole number from 1");
    refused("panel = \"eval-v1\"\n", "", "'panel' must be a text");
    refused("labeller = \"Ada\"\n", "labeller = \"Ada\"\ncolour = \"red\"\n", "unknown key 'colour'");
}

TEST(Labels, AnyVerdictIsAPersonsWorkNeverWrittenOver) {
    Labels none = labels();
    none.facts[0].verdict.reset();
    none.parts[0].verdict.reset();
    EXPECT_FALSE(none.any_verdict());
    none.sentences[0].verdict = pipeline::Issue::adds;
    EXPECT_TRUE(none.any_verdict());
    none.sentences[0].verdict.reset();
    none.parts[0].verdict = reason::Preference::tie;
    EXPECT_TRUE(none.any_verdict()) << "a part's verdict alone";
    EXPECT_TRUE(labels().any_verdict());
}

// A verdict on a part is the version shown first or second; calibration
// needs the side: A, the run; B, the run read against it.
TEST(PartLabel, ItsVerdictIsTheSideTheVersionShownIsFrom) {
    PartLabel part{.verdict = reason::Preference::first};
    EXPECT_EQ(part.winner(), reason::Winner::a) << "A's shown first";
    part.b_first = true;
    EXPECT_EQ(part.winner(), reason::Winner::b) << "B's shown first";
    part.verdict = reason::Preference::second;
    EXPECT_EQ(part.winner(), reason::Winner::a);
    part.verdict = reason::Preference::tie;
    EXPECT_EQ(part.winner(), reason::Winner::tie);
    part.verdict.reset();
    EXPECT_EQ(part.winner(), std::nullopt) << "skipped";
}

TEST(Draw, IsUniformWithoutReplacementTheSameEverywhere) {
    const std::vector<std::size_t> drawn = draw(100, 15, "r5/facts");
    EXPECT_EQ(drawn.size(), 15U);
    EXPECT_TRUE(std::ranges::is_sorted(drawn)) << "listed in the run's order";
    EXPECT_EQ(std::set<std::size_t>(drawn.begin(), drawn.end()).size(), 15U) << "without replacement";
    EXPECT_LT(drawn.back(), 100U);
    EXPECT_EQ(draw(100, 15, "r5/facts"), drawn) << "the same draw from the same name";
    EXPECT_NE(draw(100, 15, "r6/facts"), drawn) << "another name, another draw";
    EXPECT_THAT(draw(3, 15, "r5/facts"), ElementsAre(0U, 1U, 2U)) << "fewer than asked: every one";
    EXPECT_TRUE(draw(0, 15, "r5/facts").empty());

    // Every place as likely as any: 2,000 draws of 3 of 10 put each place
    // in about 600 of them.
    std::vector<int> times(10, 0);
    for (int seed = 0; seed < 2'000; ++seed) {
        for (const std::size_t index : draw(10, 3, std::format("labels-{}", seed))) {
            ++times[index];
        }
    }
    for (const int count : times) {
        EXPECT_GT(count, 500);
        EXPECT_LT(count, 700);
    }
}

TEST(BFirst, IsACoinPerPartUnderItsOwnSeed) {
    EXPECT_EQ(b_first("r5", "lecture", 0, 2), b_first("r5", "lecture", 0, 2)) << "the same everywhere";
    int heads = 0;
    for (std::size_t part = 0; part < 1'000; ++part) {
        heads += b_first("r5", "lecture", 0, part) ? 1 : 0;
    }
    EXPECT_GT(heads, 430) << "about half";
    EXPECT_LT(heads, 570);
}

}  // namespace
}  // namespace rr::eval

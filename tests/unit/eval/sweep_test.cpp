// A sweep file read strictly, and what `rreport eval sweep` prints of it.
#include <rr/eval/sweep.hpp>

#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>
#include <rr/testkit/temp_dir.hpp>

namespace rr::eval {
namespace {

using ::testing::HasSubstr;
using ::testing::Not;
using testkit::unwrap;

constexpr std::string_view kSweep = R"toml(name = "r6"
baseline = "max"

[[variant]]
label = "write-thinking"
tier = "max+write.thinking=true"
why = "Thinking while drafting: ADR-0018 keeps it off."

[[variant]]
label = "thorough"
tier = "thorough"
why = "The tier below."
)toml";

TEST(Sweep, IsItsBaselineAndItsVariantsEachALabelATierAndWhy) {
    const Sweep sweep = unwrap(parse_sweep("r6", kSweep));
    EXPECT_EQ(sweep.name, "r6");
    EXPECT_EQ(sweep.baseline, "max");
    EXPECT_EQ(sweep.replicates, 2) << "ADR-0019: sweeps run on dev with two replicates";
    ASSERT_EQ(sweep.variants.size(), 2U);
    EXPECT_EQ(sweep.variants[0], (SweepVariant{.label = "write-thinking",
                                               .tier = "max+write.thinking=true",
                                               .why = "Thinking while drafting: ADR-0018 keeps it off."}));
    EXPECT_EQ(sweep.baseline_label(), "r6-baseline");
    EXPECT_EQ(sweep.label_of(sweep.variants[1]), "r6-thorough");
    const std::string three = std::format("name = \"r6\"\nbaseline = \"max\"\nreplicates = 3\n{}",
                                          kSweep.substr(kSweep.find("[[variant]]")));
    EXPECT_EQ(unwrap(parse_sweep("r6", three)).replicates, 3);
}

TEST(Sweep, EveryMistakeIsRefusedNamingItAndWhereItIs) {
    const auto refused = [](std::string_view text, std::string_view said,
                            Errc code = Errc::invalid_argument) {
        const auto parsed = parse_sweep("s", text);
        ASSERT_FALSE(parsed.has_value()) << text;
        EXPECT_EQ(parsed.error().code(), code) << text;
        EXPECT_THAT(parsed.error().message(), HasSubstr(said)) << text;
    };
    const auto variant = [](std::string_view label, std::string_view tier) {
        return std::format("[[variant]]\nlabel = \"{}\"\ntier = \"{}\"\nwhy = \"To see.\"\n", label, tier);
    };
    constexpr std::string_view kTop = "name = \"s\"\nbaseline = \"max\"\n";
    refused(std::format("name = \"t\"\nbaseline = \"max\"\n{}", variant("a", "fast")),
            R"('name' must be the file's, "s", not "t")");
    refused(std::format("name = \"s\"\n{}", variant("a", "fast")), "'baseline' must be a text");
    refused(std::format("name = \"s\"\nbaseline = \"ultra\"\n{}", variant("a", "fast")),
            "eval/sweeps/s.toml: there is no tier 'ultra'", Errc::not_found);
    refused(std::format("{}replicates = 0\n{}", kTop, variant("a", "fast")),
            "'replicates' must be a whole number");
    refused(std::format("{}split = \"test\"\n{}", kTop, variant("a", "fast")), "unknown key 'split'");
    refused(std::string{kTop}, "no [[variant]]: a sweep compares something with its baseline");
    refused(std::format("{}{}", kTop, variant("Write", "fast")),
            "variant 1: 'label' must be lowercase letters, digits and hyphens");
    refused(std::format("{}{}", kTop, variant("baseline", "fast")), R"(and not "baseline")");
    refused(std::format("{}{}{}", kTop, variant("a", "fast"), variant("a", "thorough")),
            "variant 2: 'a' a second time: a label names one variant");
    refused(std::format("{}{}", kTop, variant("a", "max+write.drafts=two")),
            "variant 1: tier 'max+write.drafts=two', [write]: 'drafts' must be a whole number");
    refused(std::format("{}{}", kTop, variant("a", "max+write.drafts=4")),
            "variant 1: 'max+write.drafts=4' spends as the baseline, max, does");
    refused(std::format("{}{}", kTop, variant("a", "max")), "spends as the baseline");
    refused(std::format("{}[[variant]]\nlabel = \"a\"\ntier = \"fast\"\n", kTop), "'why' must be a text");
    refused(std::format("{}{}colour = \"red\"\n", kTop, variant("a", "fast")),
            "variant 1: unknown key 'colour'");
}

// The sweep runbook R6 runs is read, and each of its variants is a tier.
TEST(Sweep, TheShippedR6SweepIsReadEachVariantATier) {
    const Sweep r6 = unwrap(read_sweep(std::filesystem::path{RR_SOURCE_DIR} / "eval", "r6"));
    EXPECT_EQ(r6.baseline, "max");
    EXPECT_EQ(r6.replicates, kSweepReplicates);
    EXPECT_EQ(r6.variants.size(), 11U);
}

TEST(Sweep, IsReadFromItsFileByAName) {
    const testkit::TempDir dir;
    (void)dir.write_file("sweeps/r6.toml", kSweep);
    EXPECT_EQ(unwrap(read_sweep(dir.path(), "r6")), unwrap(parse_sweep("r6", kSweep)));
    EXPECT_EQ(sweep_file(dir.path(), "r6"), dir.path() / "sweeps" / "r6.toml");
    EXPECT_ERRC(read_sweep(dir.path(), "r7"), Errc::not_found);
    EXPECT_ERRC(read_sweep(dir.path(), "../r6"), Errc::invalid_argument) << "a name, never a path";
}

MetricRow row(std::string_view metric, double low, double high) {
    return MetricRow{.metric = *find_metric(metric),
                     .comparison = Comparison{.metric = std::string{metric},
                                              .scorer = "code@1",
                                              .videos = 4,
                                              .difference = (low + high) / 2,
                                              .interval = Interval{.low = low, .high = high}}};
}

TEST(SweepText, ListsWhatEachVariantMovedAndHowMuchAndCountsTheRest) {
    const Sweep sweep = unwrap(parse_sweep("r6", kSweep));
    const std::vector<VariantFound> found{
        VariantFound{.variant = sweep.variants[0],
                     .rows = {row("atoms_told", 0.01, 0.03), row("numbers_kept", -0.01, 0.01),
                              row("contradicts", 0.02, 0.04), row("length_ratio", -0.2, -0.1),
                              MetricRow{.metric = *find_metric("facts_told"), .why = "scorers differ"},
                              MetricRow{.metric = *find_metric("quotes_kept"),
                                        .comparison = Comparison{.metric = "quotes_kept",
                                                                 .scorer = "code@1",
                                                                 .videos = 1,
                                                                 .difference = 0.5}}}},
        VariantFound{.variant = sweep.variants[1]}};
    const std::string text = to_text(sweep, found);
    EXPECT_THAT(text, HasSubstr("sweep r6: each variant against the baseline, max (r6-baseline), on the dev "
                                "split, 2 replicates\n"));
    EXPECT_THAT(text,
                HasSubstr("\nwrite-thinking: max+write.thinking=true\n  Thinking while drafting: ADR-0018 "
                          "keeps it off.\n"));
    EXPECT_THAT(text, HasSubstr("  better  atoms_told                 +0.020   +0.010 to +0.030\n"));
    EXPECT_THAT(text, HasSubstr("  worse   contradicts                +0.030   +0.020 to +0.040\n"))
        << "more contradictions: worse, a metric better lower";
    EXPECT_THAT(text, HasSubstr("  lower   length_ratio               -0.150   -0.200 to -0.100\n"));
    EXPECT_THAT(text, Not(HasSubstr("numbers_kept"))) << "its interval holds zero";
    EXPECT_THAT(text, Not(HasSubstr("quotes_kept"))) << "one video: compared, but too few to see";
    EXPECT_THAT(text, HasSubstr("  no difference these videos see on 1 metric; 2 not compared\n"));
    EXPECT_THAT(text, HasSubstr("  every metric: rreport eval compare r6-baseline r6-write-thinking\n"));
    EXPECT_THAT(text, HasSubstr("\nthorough: thorough\n  The tier below.\n  no score yet: rreport eval score "
                                "--label r6-thorough\n"));
}

}  // namespace
}  // namespace rr::eval

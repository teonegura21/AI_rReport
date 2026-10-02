#include <rr/reason/tier.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>

namespace rr::reason {
namespace {

using ::testing::HasSubstr;
using testkit::unwrap;

constexpr std::array kStages{Stage::outline, Stage::atomize, Stage::plan, Stage::write, Stage::seams};

TEST(Tier, ThreeShipAndMaxIsTheDefault) {
    EXPECT_EQ(builtin_tiers(), (std::vector<std::string>{"fast", "thorough", "max"}));
    EXPECT_EQ(kDefaultTier, "max");
    for (const std::string& name : builtin_tiers()) {
        EXPECT_EQ(unwrap(builtin_tier(name)).name, name);
    }
    EXPECT_ERRC(builtin_tier("ultra"), Errc::not_found);
}

TEST(Tier, MaxIsWhatAdr0018Describes) {
    const Tier max = unwrap(builtin_tier("max"));
    EXPECT_EQ(max.outline, (OutlineCompute{.chapters = true,
                                           .samples = 3,
                                           .boundary_tolerance = 2,
                                           .quorum = 0.5,
                                           .thinking_budget = 16384}));
    EXPECT_EQ(max.atomize, (AtomizeCompute{.waves = Waves{.first = 3, .step = 0, .max = 3, .quorum = 0.66},
                                           .thinking = false,
                                           .repairs = 1,
                                           .recall_pass = true,
                                           .contested_to_commander = true}));
    EXPECT_EQ(max.plan, (PlanCompute{.mode = PlanMode::assign, .thinking_budget = 16384}));
    EXPECT_EQ(max.write, (WriteCompute{.drafts = 4, .thinking = false, .judge_top = 2, .repair_rounds = 3}));
    EXPECT_EQ(max.seams.mode, SeamsMode::recheck);

    EXPECT_EQ(unwrap(builtin_tier("thorough")).seams.mode, SeamsMode::rewrite);

    const Tier fast = unwrap(builtin_tier("fast"));
    EXPECT_EQ(fast.plan.mode, PlanMode::group) << "no model";
    EXPECT_EQ(fast.write.judge_top, 0);
    EXPECT_EQ(fast.seams.mode, SeamsMode::none);
}

// Each tier spends at least what the one below it does, stage by stage: a
// higher tier never buys less quality.
TEST(Tier, ComputeOnlyRisesFromFastToMax) {
    const Tier fast = unwrap(builtin_tier("fast"));
    const Tier thorough = unwrap(builtin_tier("thorough"));
    const Tier max = unwrap(builtin_tier("max"));
    const auto rises = [](const Tier& low, const Tier& high) {
        EXPECT_LE(low.outline.samples, high.outline.samples) << low.name << " → " << high.name;
        EXPECT_LE(low.outline.thinking_budget, high.outline.thinking_budget);
        EXPECT_LE(low.atomize.waves.max, high.atomize.waves.max);
        EXPECT_LE(low.atomize.repairs, high.atomize.repairs);
        EXPECT_LE(static_cast<int>(low.atomize.recall_pass), static_cast<int>(high.atomize.recall_pass));
        EXPECT_LE(static_cast<int>(low.plan.mode), static_cast<int>(high.plan.mode));
        EXPECT_LE(low.plan.thinking_budget, high.plan.thinking_budget);
        EXPECT_LE(low.write.drafts, high.write.drafts);
        EXPECT_LE(low.write.judge_top, high.write.judge_top);
        EXPECT_LE(low.write.repair_rounds, high.write.repair_rounds);
        EXPECT_LE(static_cast<int>(low.seams.mode), static_cast<int>(high.seams.mode));
    };
    rises(fast, thorough);
    rises(thorough, max);
}

TEST(Tier, AStagesCanonicalFormIsItsCacheKeyMaterial) {
    const Tier max = unwrap(builtin_tier("max"));
    EXPECT_EQ(
        max.canonical(Stage::atomize),
        R"({"contested_to_commander":true,"first":3,"max":3,"quorum":0.66,"recall_pass":true,"repairs":1,)"
        R"("step":0,"thinking":false})")
        << "compact, keys sorted";
    EXPECT_EQ(max.canonical(Stage::seams), R"({"mode":"recheck"})");
    EXPECT_EQ(max.canonical(Stage::plan), R"({"mode":"assign","thinking_budget":16384})");

    // A default written out changes nothing; a changed value changes its stage only.
    const Tier implicit = unwrap(parse_tier("custom", "[write]\ndrafts = 2\n"));
    const Tier explicit_default =
        unwrap(parse_tier("custom", "[write]\ndrafts = 2\n[seams]\nmode = \"none\"\n"));
    for (const Stage stage : kStages) {
        EXPECT_EQ(implicit.canonical(stage), explicit_default.canonical(stage)) << to_string(stage);
        const bool changed = implicit.canonical(stage) != Tier{}.canonical(stage);
        EXPECT_EQ(changed, stage == Stage::write) << to_string(stage);
    }
}

TEST(Tier, AnEmptyTierIsTheDefaults) {
    const Tier empty = unwrap(parse_tier("empty", ""));
    EXPECT_EQ(empty, (Tier{.name = "empty"}));
}

TEST(Tier, IsReadStrictly) {
    const auto refused = [](std::string_view toml, std::string_view says) {
        const Result<Tier> tier = parse_tier("custom", toml);
        ASSERT_ERRC(tier, Errc::invalid_argument) << toml;
        EXPECT_THAT(tier.error().message(), HasSubstr(says)) << toml;
        EXPECT_THAT(tier.error().message(), HasSubstr("tier 'custom'"));
    };
    refused("[polish]\n", "unknown section [polish]");
    refused("write = 3\n", "must be a section");
    refused("[write]\ndraft = 3\n", "unknown key 'draft'");
    refused("[write]\ndrafts = \"four\"\n", "'drafts' must be a whole number");
    refused("[write]\ndrafts = 0\n", "at least 1");
    refused("[write]\ndrafts = 4\njudge_top = 1\n", "'judge_top' is 0 or from 2");
    refused("[write]\ndrafts = 2\njudge_top = 3\n", "'judge_top' is 0 or from 2");
    refused("[write]\nthinking = 1\n", "true or false");
    refused("[outline]\nquorum = 1.5\n", "a share in (0, 1]");
    refused("[outline]\nquorum = nan\n", "a share in (0, 1]");
    refused("[atomize]\nquorum = 1.5\n", "a share in (0, 1]");
    refused("[atomize]\nquorum = \"half\"\n", "a share in (0, 1]");
    refused("[atomize]\nfirst = 3\nmax = 5\n", "no wave draws the rest");
    refused("[plan]\nmode = \"vote\"\n", "one of: group, assign");
    refused("[seams]\nmode = 2\n", "one of: none, rewrite, recheck");
    refused("[outline]\nboundary_tolerance = -1\n", "at least 0");
    refused("[outline\n", "line 1");
    EXPECT_OK(parse_tier("custom", "[atomize]\nquorum = 1\n")) << "a whole number is a share too";
}

TEST(Tier, AVariantIsItsBaseWithEachChangeSet) {
    const Tier max = unwrap(builtin_tier("max"));
    EXPECT_EQ(unwrap(find_tier("max")), max) << "a shipped tier, found by its name";

    const Tier thinking = unwrap(find_tier("max+write.thinking=true"));
    EXPECT_EQ(thinking.name, "max+write.thinking=true");
    Tier expected = max;
    expected.name = thinking.name;
    expected.write.thinking = true;
    EXPECT_EQ(thinking, expected);
    for (const Stage stage : {Stage::outline, Stage::atomize, Stage::plan, Stage::seams}) {
        EXPECT_EQ(thinking.canonical(stage), max.canonical(stage)) << "only the stage it changes runs again";
    }
    EXPECT_NE(thinking.canonical(Stage::write), max.canonical(Stage::write));

    const Tier several = unwrap(find_tier("thorough+write.drafts=3+atomize.quorum=0.5+plan.mode=group"));
    EXPECT_EQ(several.write.drafts, 3) << "a whole number";
    EXPECT_EQ(several.atomize.waves.quorum, 0.5) << "a number";
    EXPECT_EQ(several.plan.mode, PlanMode::group) << "a word";
    EXPECT_EQ(unwrap(find_tier("fast+seams.mode=recheck")).seams.mode, SeamsMode::recheck);
}

TEST(Tier, AVariantIsReadAsStrictlyAsATierFile) {
    const auto refused = [](std::string_view name, std::string_view says) {
        const Result<Tier> tier = find_tier(name);
        ASSERT_ERRC(tier, Errc::invalid_argument) << name;
        EXPECT_THAT(tier.error().message(), HasSubstr(says)) << name;
        EXPECT_THAT(tier.error().message(), HasSubstr(std::format("tier '{}'", name))) << "named whole";
    };
    refused("max+write.drafts=two", "'drafts' must be a whole number");
    refused("max+write.drafts=0", "at least 1");
    refused("max+write.draft=3", "unknown key 'draft'");
    refused("max+polish.drafts=3", "unknown section [polish]");
    refused("max+write.judge_top=9", "'judge_top' is 0 or from 2");
    refused("max+write.thinking=yes", "true or false");
    refused("max+write.drafts", "'write.drafts' is not stage.key=value");
    refused("max+drafts=3", "'drafts=3' is not stage.key=value");
    refused("max+.drafts=3", "is not stage.key=value");
    refused("max+write.=3", "is not stage.key=value");
    refused("max+write.drafts=", "is not stage.key=value");
    refused("max+", "'' is not stage.key=value");
    refused("max+write.drafts=2+write.drafts=3", "'write.drafts' is changed twice");
    EXPECT_ERRC(find_tier("ultra+write.drafts=2"), Errc::not_found) << "a base that is no tier";
    EXPECT_ERRC(find_tier("ultra"), Errc::not_found);
}

// The tiers compiled into the binary are the source tree's.
TEST(Tier, TheShippedTiersAreTheSourceTrees) {
    for (const std::string& name : builtin_tiers()) {
        const std::filesystem::path file =
            std::filesystem::path{RR_SOURCE_DIR} / "config" / "tiers" / (name + ".toml");
        const std::ifstream in(file);
        ASSERT_TRUE(in) << file;
        std::stringstream text;
        text << in.rdbuf();
        EXPECT_EQ(unwrap(parse_tier(name, text.str())), unwrap(builtin_tier(name))) << name;
        for (const Stage stage : kStages) {
            EXPECT_THAT(text.str(), HasSubstr(std::format("\n[{}]", to_string(stage))))
                << name << " writes out every section: a variant's change has one to set";
        }
    }
}

}  // namespace
}  // namespace rr::reason

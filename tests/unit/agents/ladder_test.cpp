#include <rr/agents/ladder.hpp>

#include <optional>
#include <vector>

#include <gtest/gtest.h>

namespace rr::agents {
namespace {

// Engine 0, the commander (Qwen); engine 1, the infantry (Gemma).
const std::vector<Engine> kEngines{
    Engine{.model = "qwen", .roles = {"commander"}, .family = "qwen"},
    Engine{.model = "gemma", .roles = {"infantry"}, .family = "gemma"},
};
constexpr std::size_t kCommander = 0;
constexpr std::size_t kInfantry = 1;

const Ladder::Holds kAlways = [](const Rung&) { return true; };

Rung rung(Step step, std::size_t engine, bool thinking) {
    return Rung{.step = step, .engine = engine, .thinking = thinking};
}

// Every rung climbed after the first, nothing refused.
std::vector<Rung> climb(Ladder& ladder, const Ladder::Holds& holds = kAlways) {
    std::vector<Rung> rungs;
    while (const std::optional<Rung> next = ladder.next(false, holds)) {
        rungs.push_back(*next);
    }
    return rungs;
}

TEST(Ladder, TheInfantryThinksThenCallsTheCommander) {
    Ladder ladder{kEngines, kInfantry, kCommander, false, true, Escalation{}};
    EXPECT_EQ(ladder.first(), rung(Step::as_ordered, kInfantry, false));
    EXPECT_EQ(climb(ladder),
              (std::vector{rung(Step::thinking, kInfantry, true), rung(Step::commander, kCommander, true)}));
    EXPECT_EQ(ladder.next(false, kAlways), std::nullopt) << "the top stays the top";
}

TEST(Ladder, TheCommanderHasNoOneAboveIt) {
    Ladder direct{kEngines, kCommander, kCommander, false, true, Escalation{}};
    EXPECT_EQ(climb(direct), (std::vector{rung(Step::thinking, kCommander, true)}));

    Ladder thinking{kEngines, kCommander, kCommander, true, true, Escalation{}};
    EXPECT_EQ(thinking.first(), rung(Step::as_ordered, kCommander, true));
    EXPECT_TRUE(climb(thinking).empty()) << "thinking again would repeat the first rung";
}

TEST(Ladder, TheCommanderIsNeverItsOwnEscalation) {
    // A commander's mission that may not think on escalation stays as it
    // asked: "the commander, thinking" would be thinking in disguise.
    Ladder ladder{kEngines, kCommander, kCommander, false, true, Escalation{.thinking = false}};
    EXPECT_TRUE(climb(ladder).empty());
}

TEST(Ladder, AMissionThatThinksSkipsTheThinkingRung) {
    Ladder ladder{kEngines, kInfantry, kCommander, true, true, Escalation{}};
    EXPECT_EQ(climb(ladder), (std::vector{rung(Step::commander, kCommander, true)}));
}

TEST(Ladder, ARefusalGoesToAnotherFamily) {
    // The infantry refuses: its thinking rung is passed over.
    Ladder infantry{kEngines, kInfantry, kCommander, false, true, Escalation{}};
    EXPECT_EQ(infantry.next(true, kAlways), rung(Step::commander, kCommander, true));
    EXPECT_EQ(infantry.refused(), (std::set<std::string>{"gemma"}));
    EXPECT_EQ(infantry.next(true, kAlways), std::nullopt) << "both families refused";

    // The commander refuses: the infantry, thinking, is the other family.
    Ladder commander{kEngines, kCommander, kCommander, false, true, Escalation{}};
    EXPECT_EQ(commander.next(true, kAlways), rung(Step::other_family, kInfantry, true));
    EXPECT_EQ(commander.next(true, kAlways), std::nullopt);
}

TEST(Ladder, OnlyARefusalOpensTheOtherFamilysRung) {
    Ladder ladder{kEngines, kCommander, kCommander, false, true, Escalation{}};
    EXPECT_EQ(ladder.next(false, kAlways), rung(Step::thinking, kCommander, true));
    EXPECT_EQ(ladder.next(true, kAlways), rung(Step::other_family, kInfantry, true))
        << "the thinking rung refused";
}

TEST(Ladder, NeverClimbsARungTwice) {
    // The infantry fails, thinks and fails, the commander refuses: Gemma
    // already thought, so there is nothing left to try.
    Ladder ladder{kEngines, kInfantry, kCommander, false, true, Escalation{}};
    EXPECT_EQ(ladder.next(false, kAlways), rung(Step::thinking, kInfantry, true));
    EXPECT_EQ(ladder.next(false, kAlways), rung(Step::commander, kCommander, true));
    EXPECT_EQ(ladder.next(true, kAlways), std::nullopt);
}

TEST(Ladder, TheOtherFamilyStartsWithTheCommander) {
    const std::vector<Engine> three{
        Engine{.model = "llama", .roles = {"reader"}, .family = "llama"},
        Engine{.model = "qwen", .roles = {"commander"}, .family = "qwen"},
        Engine{.model = "gemma", .roles = {"infantry"}, .family = "gemma"},
    };
    Ladder ladder{three, 2, 1, false, true, Escalation{.thinking = false, .commander = false}};
    EXPECT_EQ(ladder.next(true, kAlways), rung(Step::other_family, 1, true));
    EXPECT_EQ(ladder.next(true, kAlways), rung(Step::other_family, 0, true));
    EXPECT_EQ(ladder.next(true, kAlways), std::nullopt);
}

TEST(Ladder, KeepsToTheMissionsEscalation) {
    Ladder none{kEngines,   kInfantry,
                kCommander, false,
                true,       Escalation{.thinking = false, .commander = false, .other_family = false}};
    EXPECT_EQ(none.next(true, kAlways), std::nullopt);

    Ladder judge{kEngines,   kInfantry,
                 kCommander, false,
                 true,       Escalation{.thinking = true, .commander = false, .other_family = false}};
    EXPECT_EQ(climb(judge), (std::vector{rung(Step::thinking, kInfantry, true)})) << "stays in its family";
}

TEST(Ladder, PassesOverAnEngineThatCannotHoldTheMission) {
    Ladder ladder{kEngines, kInfantry, kCommander, false, true, Escalation{}};
    const Ladder::Holds infantry_only = [](const Rung& candidate) { return candidate.engine == kInfantry; };
    EXPECT_EQ(climb(ladder, infantry_only), (std::vector{rung(Step::thinking, kInfantry, true)}));

    Ladder thinking_too_long{kEngines, kInfantry, kCommander, false, true, Escalation{}};
    const Ladder::Holds no_thinking = [](const Rung& candidate) { return !candidate.thinking; };
    EXPECT_TRUE(climb(thinking_too_long, no_thinking).empty());
}

TEST(Ladder, WithoutThinkingOnlyTheModelsChange) {
    Ladder ladder{kEngines, kInfantry, kCommander, true, false, Escalation{}};
    EXPECT_EQ(ladder.first(), rung(Step::as_ordered, kInfantry, false)) << "a provider that cannot think";
    EXPECT_EQ(climb(ladder), (std::vector{rung(Step::commander, kCommander, false)}));
}

TEST(Ladder, OneEngineServingBothRolesIsItsOwnCommander) {
    const std::vector<Engine> one{
        Engine{.model = "qwen", .roles = {"commander", "infantry"}, .family = "qwen"}};
    Ladder ladder{one, 0, 0, false, true, Escalation{}};
    EXPECT_EQ(climb(ladder), (std::vector{rung(Step::thinking, 0, true)}));
    EXPECT_EQ(ladder.next(true, kAlways), std::nullopt) << "no other family on the pod";
}

}  // namespace
}  // namespace rr::agents

// The eval's frozen panel: the file shipped is the one this rreport judges
// as; every mistake in a panel file is refused where it is; a prompt or a
// procedure changed since the panel was frozen is refused, saying to freeze
// a new one; a judge is the engine serving its pinned file, and a pod that
// serves another says which to serve.
#include <rr/eval/panel.hpp>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/core/error.hpp>
#include <rr/core/file_io.hpp>
#include <rr/prompt/library.hpp>
#include <rr/testkit/result.hpp>

#include "fixtures.hpp"

namespace rr::eval {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using testkit::unwrap;

constexpr std::string_view kQwen = fixtures::kQwenFile;
constexpr std::string_view kGemma = fixtures::kGemmaFile;

const prompt::Library& prompts() {
    static const prompt::Library library = unwrap(prompt::Library::embedded());
    return library;
}

// A panel file pinning this rreport's prompts and `judges` (role, sha256).
std::string panel_text(std::string_view judges, int procedure = kJudgingProcedure) {
    std::string text = std::format("name = \"eval-v9\"\nprocedure = {}\ncalibrated = false\n", procedure);
    for (const std::string_view name : kJudgePrompts) {
        const prompt::Version& current = prompts().find(name)->current();
        text += std::format("[[prompt]]\nname = \"{}\"\nversion = {}\nhash = \"{}\"\n", name, current.number,
                            current.hash);
    }
    return text + std::string{judges};
}

std::string judge(std::string_view role, std::string_view sha256) {
    return std::format("[[judge]]\nrole = \"{}\"\nmodel = \"{}-model\"\nsha256 = \"{}\"\n", role, role,
                       sha256);
}

TEST(Panel, TheShippedPanelIsFrozenForThisRreport) {
    const std::string text =
        unwrap(read_file(std::filesystem::path{RR_SOURCE_DIR} / "eval/judges/eval-v1.toml"));
    const Panel panel = unwrap(parse_panel("eval-v1", text));
    EXPECT_EQ(panel.scorer(), "judges@eval-v1");
    EXPECT_FALSE(panel.calibrated) << "report-only until a person's labels say otherwise (P27)";
    EXPECT_THAT(
        panel.judges,
        ElementsAre(
            PanelJudge{.role = "commander", .model = "qwen3.8-27b-q4", .sha256 = std::string{kQwen}},
            PanelJudge{.role = "infantry", .model = "gemma-4-26b-a4b", .sha256 = std::string{kGemma}}));
    EXPECT_TRUE(check_panel(panel, prompts()).has_value())
        << "an eval prompt changed without a new panel: freeze eval-v2 rather than edit eval-v1";
}

TEST(Panel, EveryMistakeIsRefusedNamingItAndWhereItIs) {
    const std::string judges = judge("commander", kQwen);
    ASSERT_OK(parse_panel("eval-v9", panel_text(judges)));
    const auto refused = [&](const std::string& text) {
        const Result<Panel> parsed = parse_panel("eval-v9", text);
        EXPECT_ERRC(parsed, Errc::invalid_argument) << text;
        return parsed.has_value() ? std::string{} : parsed.error().message();
    };
    EXPECT_THAT(refused("frozen = true\n" + panel_text(judges)),
                HasSubstr("eval/judges/eval-v9.toml: unknown key 'frozen'"));
    EXPECT_THAT(refused(panel_text(judges) + "weight = 2\n"),
                HasSubstr("eval/judges/eval-v9.toml, judge 1: unknown key 'weight'"))
        << "a key after [[judge]] is the judge's";
    EXPECT_THAT(
        refused("name = \"eval-v8\"\n" + panel_text(judges).substr(panel_text(judges).find('\n') + 1)),
        HasSubstr("'name' must be the file's, \"eval-v9\", not \"eval-v8\""));
    std::string no_calibration = panel_text(judges);
    no_calibration.replace(no_calibration.find("calibrated = false"), 18, "calibrated = \"no\"");
    EXPECT_THAT(refused(no_calibration), HasSubstr("'calibrated' must be true or false"));
    std::string pipeline_prompt = panel_text(judges);
    pipeline_prompt.replace(pipeline_prompt.find("\"eval_flow\""), 11, "\"flow\"");
    EXPECT_THAT(refused(pipeline_prompt),
                HasSubstr("prompt 3: 'name' must be one of: eval_fact, eval_support, eval_flow"))
        << "never the pipeline's own judges";
    std::string twice = panel_text(judges);
    twice.replace(twice.find("\"eval_flow\""), 11, "\"eval_fact\"");
    EXPECT_THAT(refused(twice), HasSubstr("prompt 3: 'eval_fact' a second time"));
    const std::string without_flow =
        panel_text(judges).substr(0, panel_text(judges).find("[[prompt]]\nname = \"eval_flow\"")) + judges;
    EXPECT_THAT(refused(without_flow), HasSubstr("no [[prompt]] named eval_flow"));
    EXPECT_THAT(refused(panel_text(judges + judge("commander", kGemma))),
                HasSubstr("judge 2: the role 'commander' a second time"));
    EXPECT_THAT(refused(panel_text(judge("infantry", "88F4A13B"))),
                HasSubstr("judge 1: 'sha256' must be the model file's SHA-256"));
    EXPECT_THAT(refused(panel_text("")), HasSubstr("no [[judge]]"));
}

TEST(Panel, APanelFrozenWithAnotherPromptOrProcedureIsRefused) {
    const Panel panel = unwrap(parse_panel("eval-v9", panel_text(judge("commander", kQwen))));
    ASSERT_OK(check_panel(panel, prompts()));

    Panel older = panel;
    older.procedure = kJudgingProcedure + 1;
    const Result<void> procedure = check_panel(older, prompts());
    EXPECT_ERRC(procedure, Errc::conflict);
    EXPECT_THAT(
        procedure.error().message(),
        HasSubstr(
            "frozen for judging procedure 2, and this rreport judges by procedure 1: freeze a new panel"));

    Panel edited = panel;
    edited.prompts.at(1).hash = "0123456789abcdef";
    const Result<void> prompt = check_panel(edited, prompts());
    EXPECT_ERRC(prompt, Errc::conflict);
    EXPECT_THAT(prompt.error().message(),
                HasSubstr(std::format("the panel eval-v9 was frozen with eval_support@1 (01234567…), and "
                                      "this rreport asks with eval_support@1 ({}…)",
                                      prompts().find("eval_support")->current().hash.substr(0, 8))));

    Panel renumbered = panel;
    renumbered.prompts.at(0).version = 2;
    EXPECT_ERRC(check_panel(renumbered, prompts()), Errc::conflict) << "the version too, not the hash alone";
}

TEST(Panel, AJudgeIsTheEngineServingItsPinnedFile) {
    const Panel panel =
        unwrap(parse_panel("eval-v9", panel_text(judge("infantry", kGemma) + judge("commander", kQwen))));
    fixtures::Rig pod{{}, fixtures::engines_serving()};
    const std::vector<const agents::Engine*> judges = unwrap(judges_in(panel, *pod.army));
    ASSERT_EQ(judges.size(), 2U);
    EXPECT_EQ(judges[0]->model, "gemma-infantry") << "in the panel's order";
    EXPECT_EQ(judges[1]->model, "qwen-commander");

    fixtures::Rig other{
        {}, fixtures::engines_serving("aab65c67ef0dad127960efef9247f1832bca105faa1c7a052cc039b223cf86a1")};
    const auto unserved = judges_in(panel, *other.army);
    EXPECT_ERRC(unserved, Errc::needs_user);
    EXPECT_THAT(
        unserved.error().message(),
        HasSubstr("the panel eval-v9 judges with commander-model (c600de03…) as its commander, where this "
                  "pod's commander serves qwen-commander (aab65c67…): serve the panel's models"));

    const Panel by_another_role = unwrap(parse_panel("eval-v9", panel_text(judge("critic", kQwen))));
    EXPECT_THAT(judges_in(by_another_role, *pod.army).error().message(),
                HasSubstr("this pod's critic serves none"));
}

}  // namespace
}  // namespace rr::eval

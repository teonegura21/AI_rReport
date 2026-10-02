// The eval's judges, frozen (rr/eval/panel.hpp).
#include <rr/eval/panel.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

#include <rr/core/error.hpp>
#include <rr/core/file_io.hpp>
#include <rr/pipeline/asking.hpp>

#include "toml_io.hpp"

namespace rr::eval {

namespace {

bool is_sha256(std::string_view text) {
    return text.size() == 64 && std::ranges::all_of(text, [](char digit) {
               return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
           });
}

// The start of a hash, enough to tell it from another: "c600de03…".
std::string short_hash(std::string_view hash) {
    constexpr std::size_t kShown = 8;
    return hash.size() <= kShown ? std::string{hash} : std::format("{}…", hash.substr(0, kShown));
}

Result<PanelPrompt> read_prompt(detail::Table& table, const Panel& panel) {
    PanelPrompt prompt;
    RR_TRY_ASSIGN(prompt.name, table.choice("name", kJudgePrompts));
    if (std::ranges::contains(panel.prompts, prompt.name, &PanelPrompt::name)) {
        return fail(Errc::invalid_argument,
                    std::format("{}: '{}' a second time: a panel names each prompt once", table.where(),
                                prompt.name));
    }
    RR_TRY_ASSIGN(const std::int64_t version, table.integer("version", 1, 1'000'000));
    prompt.version = static_cast<int>(version);
    RR_TRY_ASSIGN(prompt.hash, table.text("hash"));
    return prompt;
}

Result<PanelJudge> read_judge(detail::Table& table, const Panel& panel) {
    PanelJudge judge;
    RR_TRY_ASSIGN(judge.role, table.text("role"));
    if (std::ranges::contains(panel.judges, judge.role, &PanelJudge::role)) {
        return fail(Errc::invalid_argument, std::format("{}: the role '{}' a second time: one judge a role",
                                                        table.where(), judge.role));
    }
    RR_TRY_ASSIGN(judge.model, table.text("model"));
    RR_TRY_ASSIGN(judge.sha256, table.text("sha256"));
    if (!is_sha256(judge.sha256)) {
        return fail(Errc::invalid_argument,
                    std::format("{}: 'sha256' must be the model file's SHA-256, 64 lowercase hex digits, as "
                                "docker/runtime/catalog.toml gives it",
                                table.where()));
    }
    return judge;
}

}  // namespace

std::string Panel::scorer() const {
    return std::format("judges@{}", name);
}

std::filesystem::path panel_file(const std::filesystem::path& eval_root, std::string_view name) {
    return eval_root / "judges" / std::format("{}.toml", name);
}

Result<Panel> parse_panel(std::string_view name, std::string_view text) {
    const std::string file = std::format("eval/judges/{}.toml", name);
    RR_TRY_ASSIGN(const toml::table document, detail::parse_toml(text, file));
    detail::Table top{document, file};
    Panel panel;
    RR_TRY_ASSIGN(panel.name, top.text("name"));
    if (panel.name != name) {
        return fail(Errc::invalid_argument,
                    std::format(R"({}: 'name' must be the file's, "{}", not "{}")", file, name, panel.name));
    }
    RR_TRY_ASSIGN(const std::int64_t procedure, top.integer("procedure", 1, 1'000'000));
    panel.procedure = static_cast<int>(procedure);
    RR_TRY_ASSIGN(panel.calibrated, top.boolean("calibrated"));
    top.allow("prompt");  // read table by table below
    top.allow("judge");
    RR_TRY(top.nothing_else());

    RR_TRY(detail::each_table(document, "prompt", file, [&](detail::Table& table) -> Result<void> {
        RR_TRY_ASSIGN(PanelPrompt prompt, read_prompt(table, panel));
        panel.prompts.push_back(std::move(prompt));
        return ok();
    }));
    for (const std::string_view needed : kJudgePrompts) {
        if (!std::ranges::contains(panel.prompts, needed, &PanelPrompt::name)) {
            return fail(Errc::invalid_argument,
                        std::format("{}: no [[prompt]] named {}: a panel pins each prompt it asks with", file,
                                    needed));
        }
    }
    RR_TRY(detail::each_table(document, "judge", file, [&](detail::Table& table) -> Result<void> {
        RR_TRY_ASSIGN(PanelJudge judge, read_judge(table, panel));
        panel.judges.push_back(std::move(judge));
        return ok();
    }));
    if (panel.judges.empty()) {
        return fail(Errc::invalid_argument, std::format("{}: no [[judge]]: a panel has one at least", file));
    }
    return panel;
}

Result<Panel> read_panel(const std::filesystem::path& eval_root, std::string_view name) {
    RR_TRY_ASSIGN(const std::string text, read_file(panel_file(eval_root, name)));
    return parse_panel(name, text);
}

Result<void> check_panel(const Panel& panel, const prompt::Library& prompts) {
    if (panel.procedure != kJudgingProcedure) {
        return fail(
            Errc::conflict,
            std::format("the panel {} was frozen for judging procedure {}, and this rreport judges by "
                        "procedure {}: freeze a new panel to judge with it",
                        panel.name, panel.procedure, kJudgingProcedure));
    }
    for (const PanelPrompt& pinned : panel.prompts) {
        const prompt::PromptInfo* info = prompts.find(pinned.name);
        if (info == nullptr) {
            return fail(Errc::conflict,
                        std::format("the panel {} asks with {}, which this rreport does not have", panel.name,
                                    pinned.name));
        }
        const prompt::Version& current = info->current();
        if (current.number != pinned.version || current.hash != pinned.hash) {
            return fail(
                Errc::conflict,
                std::format("the panel {} was frozen with {}@{} ({}), and this rreport asks with {}@{} "
                            "({}): freeze a new panel to judge with it",
                            panel.name, pinned.name, pinned.version, short_hash(pinned.hash), pinned.name,
                            current.number, short_hash(current.hash)));
        }
    }
    return ok();
}

Result<std::vector<const agents::Engine*>> judges_in(const Panel& panel, const agents::Army& army) {
    std::vector<const agents::Engine*> engines;
    std::string unserved;
    for (const PanelJudge& judge : panel.judges) {
        const agents::Engine* engine = pipeline::engine_serving(army, judge.role);
        if (engine != nullptr && engine->checksum == judge.sha256) {
            engines.push_back(engine);
            continue;
        }
        const std::string served = engine == nullptr
                                       ? std::string{"none"}
                                       : std::format("{} ({})", engine->model, short_hash(engine->checksum));
        unserved +=
            std::format("{}{} ({}) as its {}, where this pod's {} serves {}", unserved.empty() ? "" : "; ",
                        judge.model, short_hash(judge.sha256), judge.role, judge.role, served);
    }
    if (!unserved.empty()) {
        return fail(
            Errc::needs_user,
            std::format("the panel {} judges with {}: serve the panel's models to judge with it (eval/"
                        "judges/{}.toml names them), or freeze a new panel",
                        panel.name, unserved, panel.name));
    }
    return engines;
}

}  // namespace rr::eval

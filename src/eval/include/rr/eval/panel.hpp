// rr/eval/panel.hpp — the eval's judges, frozen: the prompts they are asked
// with, the models that answer, and how they are asked, each pinned.
//
// Why this exists
//   Code measures what it can (rr/eval/score.hpp); whether an article tells
//   a fact, whether the video supports a sentence, which of two versions
//   reads better, takes a reader. The eval's readers are models, and a
//   number a model gives means something only next to the model that gave
//   it, the words it was asked in, and how it was asked. So the eval's
//   judges are a panel, frozen in a file (eval/judges/<name>.toml), and
//   every score they give carries its name (scorer "judges@eval-v1"):
//     * its prompts by version and hash: eval_fact, eval_support and
//       eval_flow, never the pipeline's own judge and flow, which the
//       writer's drafts are chosen by. An eval that read with them would
//       measure how well the pipeline pleases itself (Goodhart).
//     * its judges by the role whose engine asks them and the checksum of
//       the model file it must serve: a judge is those weights, not a name.
//       Two families judge, so that no single model's leanings decide
//       (Verga et al., 2024: a panel of judges, PoLL).
//     * the judging procedure's version (kJudgingProcedure): which sentences
//       a judge is shown, and how its answers are read.
//   A change to any of these makes a new panel (eval-v2), never an edit of
//   this one: scores of different panels are never compared. A pod that
//   serves other weights cannot judge for the panel; it says which to serve.
//
//   A panel's scores stay report-only until a person's labels have shown
//   that it reads as they do (plan P27): `calibrated` says so, and the gate
//   (P25) ignores the judges' scores until it does.
//
// The file
//   name = "eval-v1"          # as the file is named
//   procedure = 1             # kJudgingProcedure when it was frozen
//   calibrated = false
//   [[prompt]]                # each of kJudgePrompts, once
//   name = "eval_fact"
//   version = 1
//   hash = "ef66…"            # the manifest's, at that version
//   [[judge]]                 # one a role, at least one
//   role = "commander"
//   model = "qwen3.8-27b-q4"  # the runtime's id for it, for people
//   sha256 = "c600…"          # the file it serves (docker/runtime/catalog.toml)
//
// Thread-safety: pure functions, but for read_panel(), which reads a file.
#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <rr/agents/army.hpp>
#include <rr/core/result.hpp>
#include <rr/prompt/library.hpp>

namespace rr::eval {

// How the judges are asked, its version: the judgement stage's
// (rr/eval/judgement.hpp). A change to which sentences a judge is shown, or
// to how its answers are read, raises it, and so needs a new panel.
inline constexpr int kJudgingProcedure = 1;

// The prompts a panel asks with, each of them once.
inline constexpr std::array<std::string_view, 3> kJudgePrompts{"eval_fact", "eval_support", "eval_flow"};

struct PanelPrompt {
    std::string name{};
    int version = 0;
    std::string hash{};

    friend bool operator==(const PanelPrompt&, const PanelPrompt&) = default;
};

struct PanelJudge {
    std::string role{};    // whose engine asks it: "commander", "infantry"
    std::string model{};   // the runtime's id for the model, for people to read
    std::string sha256{};  // the model file's: what the judge is

    friend bool operator==(const PanelJudge&, const PanelJudge&) = default;
};

struct Panel {
    std::string name{};
    int procedure = 0;
    bool calibrated = false;
    std::vector<PanelPrompt> prompts{};
    std::vector<PanelJudge> judges{};

    // "judges@eval-v1": what its scores are kept as (rr/eval/run.hpp).
    [[nodiscard]] std::string scorer() const;

    friend bool operator==(const Panel&, const Panel&) = default;
};

[[nodiscard]] std::filesystem::path panel_file(const std::filesystem::path& eval_root, std::string_view name);

// `text`, the panel `name`'s file, read strictly: a key misspelt, a value
// missing or of the wrong kind, a prompt not of kJudgePrompts or named twice,
// a role named twice, a checksum that is not 64 lowercase hex digits, each
// Errc::invalid_argument saying where.
[[nodiscard]] Result<Panel> parse_panel(std::string_view name, std::string_view text);

// The panel `name` of the eval at `eval_root`. Errc::not_found when there is
// none; parse_panel()'s errors.
[[nodiscard]] Result<Panel> read_panel(const std::filesystem::path& eval_root, std::string_view name);

// Whether this rreport judges as `panel` was frozen to: its procedure, and
// each of its prompts as `prompts` has it now (its current version and hash).
// Errc::conflict otherwise, naming what changed and what to do: freeze a new
// panel.
[[nodiscard]] Result<void> check_panel(const Panel& panel, const prompt::Library& prompts);

// The engine of `army` that serves each of `panel`'s judges, in the panel's
// order: its role's, serving the pinned file. Errc::needs_user naming each
// judge the pod does not serve as pinned, and the model to serve.
[[nodiscard]] Result<std::vector<const agents::Engine*>> judges_in(const Panel& panel,
                                                                   const agents::Army& army);

}  // namespace rr::eval

// eval_commands.hpp — `rreport eval`: the evaluation's commands (ADR-0019,
// docs/design/eval.md): its videos fetched and annotated; runs taken through
// the stages, scored by code and by the frozen judges, exported, compared,
// gated, swept and drawn on one page.
#pragma once

#include <memory>
#include <optional>

#include "cli.hpp"

namespace CLI {
class App;
}  // namespace CLI

namespace rr::cli {

class EvalCommands {
public:
    // Adds `eval` and its subcommands to `app`. Their options are bound to
    // this object, which outlives the parse: never copied, never moved.
    explicit EvalCommands(CLI::App& app);
    ~EvalCommands();
    EvalCommands(const EvalCommands&) = delete;
    EvalCommands& operator=(const EvalCommands&) = delete;
    EvalCommands(EvalCommands&&) = delete;
    EvalCommands& operator=(EvalCommands&&) = delete;

    // The exit code of the eval command parsed; none when another was.
    [[nodiscard]] std::optional<int> run(const Global& global, const proc::TerminationSignals& signals) const;

private:
    struct Commands;  // the options, and the subcommands they belong to
    std::unique_ptr<Commands> commands_;
};

}  // namespace rr::cli

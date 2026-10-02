// rr/eval/sweep.hpp — variants of the pipeline run side by side, each
// compared with one baseline: `rreport eval sweep`.
//
// Why this exists
//   Choosing a default (runbook R6) asks the same question many times: is
//   this variant better than the baseline, by how much, at what cost? Each
//   answer is a run and a comparison (rr/eval/compare.hpp); a sweep file
//   writes the questions down once, so that they are asked alike, read
//   later by someone else, and taken up where they stopped.
//
//   A variant is a tier, shipped or changed by name
//   ("max+write.thinking=true", rr/reason/tier.hpp): the ablations (a
//   stage's step turned off), the A/B tests (thinking on while drafting),
//   the tiers against each other. Every run of a sweep draws under the same
//   salts, replicates from 0, so variants differ by their change and not by
//   the dice, and stages a variant leaves alone are the baseline's own
//   (ADR-0019, ADR-0020). A sweep explores: it runs on the dev split, with
//   two replicates by default (ADR-0019); the test split is kept for gates.
//
// The file (eval/sweeps/<name>.toml), read strictly:
//     name = "r6"
//     baseline = "max"
//     replicates = 2                  # optional; at least 1
//     [[variant]]
//     label = "write-thinking"
//     tier = "max+write.thinking=true"
//     why = "thinking while drafting: does it help here?"
//   Its runs are labelled `<name>-baseline` and `<name>-<label>`.
//
// Thread-safety: pure functions, but for read_sweep(), which reads a file.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/eval/compare.hpp>

namespace rr::eval {

struct SweepVariant {
    std::string label{};  // lowercase letters, digits and hyphens, as a video's id
    std::string tier{};   // a tier, or a variant of one (rr/reason/tier.hpp)
    std::string why{};    // what it tries, for whoever reads the sweep later

    friend bool operator==(const SweepVariant&, const SweepVariant&) = default;
};

inline constexpr int kSweepReplicates = 2;  // ADR-0019: sweeps run on dev with two replicates

struct Sweep {
    std::string name{};
    std::string baseline{};  // the tier every variant is compared with
    int replicates = kSweepReplicates;
    std::vector<SweepVariant> variants{};

    // Its runs' labels: "r6-baseline", "r6-write-thinking".
    [[nodiscard]] std::string baseline_label() const;
    [[nodiscard]] std::string label_of(const SweepVariant& variant) const;

    friend bool operator==(const Sweep&, const Sweep&) = default;
};

[[nodiscard]] std::filesystem::path sweep_file(const std::filesystem::path& eval_root, std::string_view name);

// `text`, the sweep `name`'s file. Errc::invalid_argument, saying where, for
// a name not the file's, a key unknown or missing, a label that is no id or
// is "baseline" or comes twice, a tier that is none (or Errc::not_found for
// one whose base is no tier), a variant whose settings are the baseline's
// (it would compare the baseline with itself), fewer than one replicate, or
// no variant at all.
[[nodiscard]] Result<Sweep> parse_sweep(std::string_view name, std::string_view text);

// The sweep `name` of `eval_root`, as parse_sweep() reads it.
// Errc::invalid_argument for a name that is no sweep's (never a path);
// Errc::not_found for no such file.
[[nodiscard]] Result<Sweep> read_sweep(const std::filesystem::path& eval_root, std::string_view name);

// What a sweep found of one variant: its rows against the baseline
// (compare_runs()).
struct VariantFound {
    SweepVariant variant{};
    std::vector<MetricRow> rows{};
};

// As `rreport eval sweep` prints it: for each variant, what it changes and
// why, then each metric whose interval leaves out zero, the way it moved
// and by how much; how many metrics show no difference, and how many could
// not be compared; and the comparison that shows every row.
[[nodiscard]] std::string to_text(const Sweep& sweep, const std::vector<VariantFound>& found);

}  // namespace rr::eval

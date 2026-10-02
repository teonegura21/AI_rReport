// Variants of the pipeline compared with one baseline (rr/eval/sweep.hpp).
#include <rr/eval/sweep.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <utility>

#include <rr/core/error.hpp>
#include <rr/core/file_io.hpp>
#include <rr/eval/set.hpp>
#include <rr/reason/tier.hpp>

#include "shown.hpp"
#include "toml_io.hpp"

namespace rr::eval {

namespace {

constexpr std::string_view kBaseline = "baseline";
constexpr std::int64_t kMostReplicates = 100;

// The tier `name` names, its message saying where it is named.
Result<reason::Tier> tier_named(std::string_view name, std::string_view where) {
    Result<reason::Tier> tier = reason::find_tier(name);
    if (!tier.has_value()) {
        return fail(tier.error().code(), std::format("{}: {}", where, tier.error().message()));
    }
    return tier;
}

// Whether two tiers spend alike, whatever they are called: every stage's
// settings the same, so every stage's key.
bool spend_alike(reason::Tier a, const reason::Tier& b) {
    a.name = b.name;
    return a == b;
}

// A [[variant]] of a sweep file, added to `sweep`, whose baseline spends as
// `baseline` does.
Result<void> parse_variant(detail::Table& table, const reason::Tier& baseline, Sweep& sweep) {
    SweepVariant variant;
    RR_TRY_ASSIGN(variant.label, table.text("label"));
    if (!valid_video_id(variant.label) || variant.label == kBaseline) {
        return fail(
            Errc::invalid_argument,
            std::format(
                R"({}: 'label' must be lowercase letters, digits and hyphens, and not "{}": not "{}")",
                table.where(), kBaseline, variant.label));
    }
    if (std::ranges::contains(sweep.variants, variant.label, &SweepVariant::label)) {
        return fail(Errc::invalid_argument, std::format("{}: '{}' a second time: a label names one variant",
                                                        table.where(), variant.label));
    }
    RR_TRY_ASSIGN(variant.tier, table.text("tier"));
    RR_TRY_ASSIGN(const reason::Tier tier, tier_named(variant.tier, table.where()));
    if (spend_alike(tier, baseline)) {
        return fail(
            Errc::invalid_argument,
            std::format("{}: '{}' spends as the baseline, {}, does: it would compare the baseline with "
                        "itself",
                        table.where(), variant.tier, sweep.baseline));
    }
    RR_TRY_ASSIGN(variant.why, table.text("why"));
    RR_TRY(table.nothing_else());
    sweep.variants.push_back(std::move(variant));
    return ok();
}

// "  better  facts_told             +0.031   +0.012 to +0.050"
std::string moved_line(const MetricRow& row, const Comparison& compared) {
    const std::string interval = compared.interval.has_value()
                                     ? std::format("{} to {}", detail::signed_shown(compared.interval->low),
                                                   detail::signed_shown(compared.interval->high))
                                     : std::string{};
    return std::format("  {:<8}{:<24}{:>9}   {}\n", to_string(seen_in(row)), row.metric.name,
                       detail::signed_shown(compared.difference), interval);
}

std::string variant_text(const Sweep& sweep, const VariantFound& found) {
    std::string text =
        std::format("\n{}: {}\n  {}\n", found.variant.label, found.variant.tier, found.variant.why);
    if (found.rows.empty()) {
        return text +
               std::format("  no score yet: rreport eval score --label {}\n", sweep.label_of(found.variant));
    }
    std::size_t alike = 0;
    std::size_t not_compared = 0;
    for (const MetricRow& row : found.rows) {
        const Seen seen = seen_in(row);
        if (seen == Seen::none) {
            ++alike;
        } else if (seen == Seen::unknown || !row.comparison.has_value()) {
            // Too few videos to see, or not compared at all (which seen_in()
            // calls unknown too: the second test guards the dereference below).
            ++not_compared;
        } else {
            text += moved_line(row, *row.comparison);
        }
    }
    text += std::format("  no difference these videos see on {} metric{}", alike, alike == 1 ? "" : "s");
    if (not_compared > 0) {
        text += std::format("; {} not compared", not_compared);
    }
    return text + std::format("\n  every metric: rreport eval compare {} {}\n", sweep.baseline_label(),
                              sweep.label_of(found.variant));
}

}  // namespace

std::string Sweep::baseline_label() const {
    return std::format("{}-{}", name, kBaseline);
}

std::string Sweep::label_of(const SweepVariant& variant) const {
    return std::format("{}-{}", name, variant.label);
}

std::filesystem::path sweep_file(const std::filesystem::path& eval_root, std::string_view name) {
    return eval_root / "sweeps" / std::format("{}.toml", name);
}

Result<Sweep> parse_sweep(std::string_view name, std::string_view text) {
    const std::string file = std::format("eval/sweeps/{}.toml", name);
    RR_TRY_ASSIGN(const toml::table document, detail::parse_toml(text, file));
    detail::Table top{document, file};
    Sweep sweep;
    RR_TRY_ASSIGN(sweep.name, top.text("name"));
    if (sweep.name != name) {
        return fail(Errc::invalid_argument,
                    std::format(R"({}: 'name' must be the file's, "{}", not "{}")", file, name, sweep.name));
    }
    RR_TRY_ASSIGN(sweep.baseline, top.text("baseline"));
    RR_TRY_ASSIGN(const reason::Tier baseline, tier_named(sweep.baseline, file));
    RR_TRY_ASSIGN(const std::optional<std::int64_t> replicates,
                  top.optional_integer("replicates", 1, kMostReplicates));
    sweep.replicates = static_cast<int>(replicates.value_or(kSweepReplicates));
    top.allow("variant");  // read table by table below
    RR_TRY(top.nothing_else());
    RR_TRY(detail::each_table(document, "variant", file,
                              [&](detail::Table& table) { return parse_variant(table, baseline, sweep); }));
    if (sweep.variants.empty()) {
        return fail(Errc::invalid_argument,
                    std::format("{}: no [[variant]]: a sweep compares something with its baseline", file));
    }
    return sweep;
}

Result<Sweep> read_sweep(const std::filesystem::path& eval_root, std::string_view name) {
    if (!valid_video_id(name)) {
        return fail(Errc::invalid_argument,
                    std::format("'{}' is no sweep's name: lowercase letters, digits and hyphens", name));
    }
    RR_TRY_ASSIGN(const std::string text, read_file(sweep_file(eval_root, name)));
    return parse_sweep(name, text);
}

std::string to_text(const Sweep& sweep, const std::vector<VariantFound>& found) {
    std::string text = std::format(
        "sweep {}: each variant against the baseline, {} ({}), on the dev split, {} replicate{}\n",
        sweep.name, sweep.baseline, sweep.baseline_label(), sweep.replicates,
        sweep.replicates == 1 ? "" : "s");
    for (const VariantFound& one : found) {
        text += variant_text(sweep, one);
    }
    return text;
}

}  // namespace rr::eval

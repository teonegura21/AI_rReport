// rr/reason/tier.hpp — how much compute each stage spends: the tiers fast,
// thorough and max.
//
// Why this exists
//   Test-time compute is how a fixed model gives better answers, and each way
//   of spending it multiplies GPU time (ADR-0018). A tier names a setting of
//   every stage's knobs at once: its samples and waves, whether it thinks
//   and how long, its repair rounds, its judges. So quality can be charted
//   against cost (ADR-0019), and one word says how hard a job works.
//   `max` is the default.
//
//   The tiers are files (config/tiers/<name>.toml, compiled in). Reading one
//   is strict: an unknown section or key, a value of the wrong type or out
//   of range is an error that names it. A key left out takes the default
//   below. Each stage's settings enter its cache key in canonical form
//   (Tier::canonical()), with the defaults filled in. So changing one value
//   re-runs exactly that stage and the stages after it, and writing out a
//   default changes nothing.
//
// Thread-safety: plain values and pure functions.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/reason/vote.hpp>

namespace rr::reason {

enum class Stage : std::uint8_t {
    outline,
    atomize,
    plan,
    write,
    seams
};

[[nodiscard]] std::string_view to_string(Stage stage) noexcept;

// The commander, thinking: the article's sections and the video brief.
struct OutlineCompute {
    bool chapters = true;        // use the video's own chapters when it has them
    int samples = 1;             // outlines drawn when it has none
    int boundary_tolerance = 2;  // sentences within which outlines' boundaries agree
    double quorum = 0.5;         // the share of outlines a boundary needs
    int thinking_budget = 8192;

    friend bool operator==(const OutlineCompute&, const OutlineCompute&) = default;
};

// The infantry, in closed-world windows of the transcript.
struct AtomizeCompute {
    Waves waves{};
    bool thinking = false;
    int repairs = 1;
    bool recall_pass = false;             // re-ask the sentences that atoms barely cover
    bool contested_to_commander = false;  // what samples still disagree on, the commander decides

    friend bool operator==(const AtomizeCompute&, const AtomizeCompute&) = default;
};

enum class PlanMode : std::uint8_t {
    group,  // deterministic grouping: no model
    assign  // the commander plans, and assigns every atom to a paragraph
};

struct PlanCompute {
    PlanMode mode = PlanMode::group;
    int thinking_budget = 8192;

    friend bool operator==(const PlanCompute&, const PlanCompute&) = default;
};

// The commander drafts each section.
struct WriteCompute {
    int drafts = 1;
    bool thinking = false;
    int judge_top = 0;  // the drafts with fewest ledger violations a judge compares (0: none)
    int repair_rounds = 1;

    friend bool operator==(const WriteCompute&, const WriteCompute&) = default;
};

enum class SeamsMode : std::uint8_t {
    none,     // sections are joined as written
    rewrite,  // the paragraphs where sections meet are rewritten
    recheck   // rewritten, then checked again
};

struct SeamsCompute {
    SeamsMode mode = SeamsMode::none;

    friend bool operator==(const SeamsCompute&, const SeamsCompute&) = default;
};

struct Tier {
    std::string name{};
    OutlineCompute outline{};
    AtomizeCompute atomize{};
    PlanCompute plan{};
    WriteCompute write{};
    SeamsCompute seams{};

    // `stage`'s settings as compact JSON, keys sorted, every value present:
    // what enters the stage's cache key.
    [[nodiscard]] std::string canonical(Stage stage) const;

    friend bool operator==(const Tier&, const Tier&) = default;
};

inline constexpr std::string_view kDefaultTier = "max";

// A tier from its TOML text, named `name`. Errc::invalid_argument naming
// what is wrong (see the header comment).
[[nodiscard]] Result<Tier> parse_tier(std::string_view name, std::string_view toml);

// The tiers rReport ships, cheapest first: fast, thorough, max.
[[nodiscard]] std::vector<std::string> builtin_tiers();

// A shipped tier. Errc::not_found for a name that is not one.
[[nodiscard]] Result<Tier> builtin_tier(std::string_view name);

}  // namespace rr::reason

// rr/knowledge/plan.hpp — how the article retells the atoms: each section's
// heading, its paragraphs in the order told, the atoms each tells, and the
// atoms only another tells.
//
// Why this exists
//   The writer drafts the article a section at a time (plan P20), and Ledger
//   B then holds it to every atom: each told, or declared a duplicate of an
//   atom that is (ADR-0005, rule 6). The plan decides that once, before any
//   prose: which paragraph tells which atoms, in what order, and which atoms
//   only say again what another says. Deciding it apart from the writing
//   keeps every decision checkable by code: a paragraph's atoms are a closed
//   list, and a writer handed them cannot drop one unseen.
//   The plan stage writes it (rr/pipeline/plan_stage.hpp): the commander,
//   thinking over one section's atoms at a time, or, at the `fast` tier and
//   wherever no plan could be had, the atoms grouped in the order said.
//
// What reading checks
//   Beyond the schema (schemas/artifacts/plan.json): sections in the order
//   of their first sentences, each once; an atom told once, or declared a
//   duplicate, never both; a duplicate not of itself, and of an atom a
//   paragraph tells. A section may have no paragraph: one whose sentences
//   are all filler has nothing to tell. Whether the plan holds every atom of
//   the ledger, each in the section its first sentence is in, is a matter
//   between three artifacts: plan_problems() says it.
//
// Thread-safety: plain values and pure functions.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/json/json.hpp>
#include <rr/knowledge/atoms.hpp>
#include <rr/knowledge/ids.hpp>
#include <rr/knowledge/outline.hpp>

namespace rr::knowledge {

struct Paragraph {
    // What a reader learns from it, in a sentence; none for a paragraph
    // grouped in the order said, which no model planned.
    std::optional<std::string> purpose{};
    std::vector<AtomId> atoms{};  // in the order told; one at least

    friend bool operator==(const Paragraph&, const Paragraph&) = default;
};

// Who planned a section.
enum class PlannedBy : std::uint8_t {
    commander,  // thinking over the section's atoms
    grouping,   // the atoms grouped in the order said: no model
};

[[nodiscard]] std::string_view to_string(PlannedBy by) noexcept;

struct SectionPlan {
    SentenceId first;  // the outline's section it plans, by its first sentence
    std::string heading{};
    std::vector<Paragraph> paragraphs{};  // in the order told
    PlannedBy planned_by = PlannedBy::grouping;

    friend bool operator==(const SectionPlan&, const SectionPlan&) = default;
};

// An atom that only says again what another says, and is told by it.
struct Duplicate {
    AtomId atom;
    AtomId of;
    std::optional<std::string> because{};  // why, as the planner said it; none: no model said

    friend bool operator==(const Duplicate&, const Duplicate&) = default;
};

struct Plan {
    std::vector<SectionPlan> sections{};  // in the order of their first sentences
    std::vector<Duplicate> duplicates{};  // in the order declared

    friend bool operator==(const Plan&, const Plan&) = default;
};

[[nodiscard]] json::Json to_json(const Plan& plan);

// Errc::invalid_argument when `document` breaks the schema or the rules in
// the header comment.
[[nodiscard]] Result<Plan> plan_from_json(const json::Json& document);

// Where `plan` and the ledger and outline it was made for disagree, said as
// what to correct: a section of the outline it does not plan, or one it
// plans that the outline has not; an atom of the ledger it neither tells
// nor declares a duplicate, or one it names that the ledger has not; an
// atom told in a section other than its first sentence's.
[[nodiscard]] std::vector<std::string> plan_problems(const Plan& plan, const AtomLedger& ledger,
                                                     const Outline& outline);

}  // namespace rr::knowledge

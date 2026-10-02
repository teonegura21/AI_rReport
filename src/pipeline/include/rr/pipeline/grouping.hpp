// rr/pipeline/grouping.hpp — a section's atoms grouped into paragraphs in
// the order said, without a model.
//
// Why this exists
//   The plan stage asks the commander how each section is told
//   (rr/pipeline/plan_stage.hpp). At the `fast` tier it does not ask, and
//   wherever no plan could be had it cannot: the atoms are then told in the
//   order said, a paragraph at a time. Grouping is a floor, not a plan: it
//   keeps every atom and the talk's order, and leaves the article's shape to
//   the writer.
//
// How
//   A new paragraph starts after kGroupedAtoms atoms, and wherever the
//   speaker or the register changes: an answer to a question, a story told
//   aside, a joke, each starts a paragraph of its own. A grouped paragraph
//   has no purpose: no model said what a reader learns from it.
//
// Thread-safety: pure functions.
#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include <rr/knowledge/atoms.hpp>
#include <rr/knowledge/plan.hpp>

namespace rr::pipeline {

// The most atoms a grouped paragraph tells: about a paragraph's sentences.
inline constexpr std::size_t kGroupedAtoms = 6;

// `atoms`, in the order said, as paragraphs (see the header comment); none
// for no atoms.
[[nodiscard]] std::vector<knowledge::Paragraph> grouped(std::span<const knowledge::Atom* const> atoms);

}  // namespace rr::pipeline

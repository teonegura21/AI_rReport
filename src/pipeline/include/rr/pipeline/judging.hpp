// rr/pipeline/judging.hpp — a draft's sentences read against their atoms,
// and two drafts compared for how they read: by a model of another family
// than the one that wrote them.
//
// Why this exists
//   Ledger B checks what code can: every atom told, its numbers, names,
//   terms and words to quote kept, every quotation verbatim. It cannot tell
//   a sentence that adds a cause no atom gives, hedges what was said for
//   sure, states for sure what was hedged, or gives a statement to someone
//   else. A judge can, when it is given the evidence first: the cited
//   atoms' words, quoted before any verdict (Chain-of-Verification; the
//   answer's fields are in that order, rr/json/schema.hpp). It is of
//   another family than the writer, since a model favours its own kind
//   (Verga et al., 2024; PoLL), and it compares two drafts in both orders,
//   since it also favours a side (rr/reason/pairwise.hpp).
//
//   What the judge says is a reading, never a check: it decides between
//   drafts that already passed every check, and what it finds in the draft
//   kept is published as its reading (ADR-0005: never hidden).
//
// How
//   * sentences_of() numbers a draft's sentences for the judge, each as
//     written, markers and all, so it sees which atoms each one cites.
//   * judgment_problems() checks an answer against the sentences asked:
//     each judged once, in order. judgment_of() reads it.
//   * A flow judge answers "first", "second" or "tie" (prompts/flow);
//     preference_of() reads it for reason::swapped().
//
// Thread-safety: pure functions.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rr/json/json.hpp>
#include <rr/reason/pairwise.hpp>

namespace rr::pipeline {

// What a judge reads a sentence as doing (prompts/judge, its answer's
// `issue`).
enum class Issue : std::uint8_t {
    none,           // it says what its atoms say, and only that
    adds,           // something none of its atoms says
    softens,        // hedges or weakens what an atom states as sure or strong
    intensifies,    // states as sure or strong what an atom hedges
    misattributes,  // gives a statement to another, or drops who said it
    contradicts,    // the opposite, or another number, name or event
};

[[nodiscard]] std::string_view to_string(Issue issue) noexcept;
// The inverse of to_string(); none for anything else.
[[nodiscard]] std::optional<Issue> issue_from_string(std::string_view name) noexcept;
// "adding what its atoms do not say": a reading, for a finding.
[[nodiscard]] std::string_view reading_of(Issue issue) noexcept;

// A sentence as the judge read it.
struct Judged {
    std::size_t sentence = 0;  // 1, 2, … in the draft
    std::string evidence{};
    std::optional<std::string> reading{};
    Issue issue = Issue::none;

    friend bool operator==(const Judged&, const Judged&) = default;
};

// The sentences of `draft`'s paragraphs, in order, each as written, markers
// and all: what a judge is shown, numbered from 1.
[[nodiscard]] std::vector<std::string> sentences_of(std::string_view draft);

// What is wrong with a judge's `answer` (its schema already held) about
// `count` sentences: each judged once, in order, and none else.
[[nodiscard]] std::vector<std::string> judgment_problems(const json::Json& answer, std::size_t count);

// The sentences of a checked answer, in order.
[[nodiscard]] std::vector<Judged> judgment_of(const json::Json& answer);

// The preference of a flow judge's checked answer (prompts/flow).
[[nodiscard]] reason::Preference preference_of(const json::Json& answer);

}  // namespace rr::pipeline

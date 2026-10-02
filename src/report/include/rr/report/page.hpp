// rr/report/page.hpp — a job's article as one web page: the article
// retold, every sentence linked to the moment of the video it tells, and
// the Fidelity Report that says what was kept, what was left out and why.
//
// Why this exists
//   The artifacts are the truth (ADR-0004); a page is a view of them,
//   rendered whenever it is asked for, never stored as a result: a change to
//   how pages look runs no job again. Every figure on it is computed from
//   the artifacts: a sentence's time is the first sentence of the first atom
//   it cites (ADR-0006: no model gives a time), and the coverage is Ledger A
//   and Ledger B checked again (rr/fidelity/checks.hpp). What the stages
//   could not do, their findings, is shown with the rest (ADR-0005: never
//   hidden).
//
// How
//   render_page() writes one self-contained HTML file: its styles and its
//   one short script inline, nothing fetched but the video's own player.
//     1. The video: its title, channel, length and link; the job and its
//        tier; the player.
//     2. Contents: each section's heading and start.
//     3. The article: each section under its heading and its start, each
//        sentence starting the player at its moment when clicked. Markers
//        become small links to the atoms they cite, in the ledger below; a
//        switch shows or hides them.
//     4. The Fidelity Report: what was kept, against M1's targets; what is
//        not told, and why (every filler sentence, every duplicate); what
//        the stages could not do.
//     5. The ledger and the transcript, folded.
//   A job not done renders what it has, and says what is missing.
//
// Safety: everything on the page came from a video someone else made. Every
// text is escaped (escaped()), in elements and attributes alike; the
// article is rendered from its parse (rr/knowledge/article.hpp), never
// passed through as HTML; the only links built from data are the video's,
// from an id of YouTube's form, eleven of A–Z a–z 0–9 _ -.
//
// Thread-safety: pure functions.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rr/fidelity/finding.hpp>
#include <rr/knowledge/article.hpp>
#include <rr/knowledge/atoms.hpp>
#include <rr/knowledge/outline.hpp>
#include <rr/knowledge/plan.hpp>
#include <rr/knowledge/transcript.hpp>
#include <rr/media/video.hpp>

namespace rr::report {

// What a stage could not do, as it published it.
struct StageFindings {
    std::string stage{};
    std::vector<std::string> findings{};

    friend bool operator==(const StageFindings&, const StageFindings&) = default;
};

// A job's artifacts, as far as it got, and what its stages could not do.
struct Contents {
    std::string job{};                    // its id
    std::string tier{};                   // "max"
    std::optional<media::Video> video{};  // a caption file's: its title alone
    std::optional<knowledge::Transcript> transcript{};
    std::optional<knowledge::Outline> outline{};
    std::optional<knowledge::AtomLedger> ledger{};
    std::optional<knowledge::Plan> plan{};
    std::optional<knowledge::Article> article{};  // the seams stage's, else the write stage's
    std::vector<StageFindings> findings{};        // in the order the stages ran

    friend bool operator==(const Contents&, const Contents&) = default;
};

// The page (see the header comment).
[[nodiscard]] std::string render_page(const Contents& contents);

// Ledger A, and where there is an article Ledger B, checked again as the
// stages check them (rr/fidelity/checks.hpp), the video's lexicon known and
// the plan's duplicates excused: what the Fidelity Report shows, and what the
// eval scores (rr/eval/score.hpp), so the two always agree. None before
// Ledger A is written.
[[nodiscard]] std::optional<fidelity::Report> checked(const Contents& contents);

// `text` safe as HTML text and attribute values: & < > " ' escaped.
[[nodiscard]] std::string escaped(std::string_view text);

}  // namespace rr::report

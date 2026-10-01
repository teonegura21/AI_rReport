// rr/eval/gold.hpp — what a video says that an article must not lose: its
// gold annotation.
//
// Why this exists
//   The eval's recall metrics count what an article kept of what its video
//   says (ADR-0019). What a video says is written down once, by a person, in
//   eval/gold/<id>.toml. Models draft it, "silver" (`rreport eval annotate`,
//   rr/eval/annotate.hpp), and a person corrects it into gold. The file says
//   who did, and the eval scores only gold that someone reviewed.
//
// Anchored to the timeline (ADR-0009)
//   A transcript's sentence numbers change whenever its segmentation
//   improves, so gold never names an S#. Each fact holds the stretch of the
//   video that says it (start, end) and a quote of the words that say it,
//   and is found again in any transcript by both: the quote, said verbatim
//   within its stretch (gold_problems()).
//
// What it holds
//   * facts: what is said, as a claim in one sentence, with the quote it
//     rests on; core when an article without it misses the point, detail
//     otherwise;
//   * must-quotes: words an article must quote, not paraphrase: a definition,
//     a line the speaker stresses;
//   * terms: the video's own words for things, which an article must keep.
//
// The file is written for people: times as players show them ("3:04.2"), a
// table per fact, in the order said, and a comment that explains the rest.
// It is read strictly (an unknown key, or a fact without its importance, is
// an error naming it), so a slip while correcting it is caught.
//
// Thread-safety: plain values and pure functions.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/core/time_span.hpp>
#include <rr/knowledge/transcript.hpp>

namespace rr::eval {

enum class Importance : std::uint8_t {
    core,    // an article without it misses the point
    detail,  // it belongs in a complete article
};

enum class GoldStatus : std::uint8_t {
    silver,  // drafted by models, not yet reviewed
    gold,    // corrected by a person, who is named
};

[[nodiscard]] std::string_view to_string(Importance importance) noexcept;
[[nodiscard]] std::string_view to_string(GoldStatus status) noexcept;

struct Fact {
    std::string id{};  // "F1", "F2", …: unique, and kept when facts are corrected
    Millis start{0};
    Millis end{0};
    Importance importance = Importance::core;
    std::string quote{};  // the words that say it, as said
    std::string claim{};  // what they say, in one sentence
    // How many of the drafts found it: for the reviewer, who doubts a fact
    // one draft in three found. 0: added by the other family, or by a person.
    int found = 0;

    friend bool operator==(const Fact&, const Fact&) = default;
};

struct MustQuote {
    Millis start{0};
    Millis end{0};
    std::string quote{};

    friend bool operator==(const MustQuote&, const MustQuote&) = default;
};

struct Term {
    std::string term{};
    std::string meaning{};  // empty: the video does not say

    friend bool operator==(const Term&, const Term&) = default;
};

struct Gold {
    std::string video{};     // the set's id for it
    std::string snapshot{};  // the snapshot it was annotated on (rr/eval/snapshot.hpp)
    GoldStatus status = GoldStatus::silver;
    std::string reviewer{};    // who corrected it: required for gold
    std::string drafted_by{};  // the prompts and models that drafted it
    std::vector<Fact> facts{};
    std::vector<MustQuote> must_quotes{};
    std::vector<Term> terms{};

    friend bool operator==(const Gold&, const Gold&) = default;
};

// eval_root/gold/<id>.toml
[[nodiscard]] std::filesystem::path gold_file(const std::filesystem::path& eval_root, std::string_view id);

// Gold from its TOML text (see the header comment); `file` names it in
// messages. Errc::invalid_argument naming what is wrong, and where.
[[nodiscard]] Result<Gold> parse_gold(std::string_view toml, std::string_view file);

// The file a person reads and corrects: parse_gold() reads it back equal.
[[nodiscard]] std::string to_toml(const Gold& gold);

// A time as players show it, to the millisecond where it has one: "3:04",
// "3:04.25", "1:02:03.5".
[[nodiscard]] std::string clock_time(Millis at);

// How far around its stretch a quote may be found: captions' times are
// approximate, and a person reads them off a player.
inline constexpr Millis kAnchorSlack{2000};

// Whether `quote` is said, verbatim (rr/text/align.hpp: case, accents and
// punctuation aside), in the sentences of `transcript` said within
// [start - kAnchorSlack, end + kAnchorSlack].
[[nodiscard]] bool said_there(std::string_view quote, Millis start, Millis end,
                              const knowledge::Transcript& transcript);

// What is wrong with `gold`, one line each, said as what to correct; empty
// when nothing is:
//   * a fact or must-quote that ends before it starts, or whose quote is not
//     said within its stretch (said_there());
//   * gold without a reviewer.
[[nodiscard]] std::vector<std::string> gold_problems(const Gold& gold,
                                                     const knowledge::Transcript& transcript);

}  // namespace rr::eval

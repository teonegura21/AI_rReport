// rr/eval/calibration.hpp — whether the eval's judges read as a person does:
// what `rreport eval calibrate` measures (ADR-0019).
//
// Why this exists
//   A judged metric is only as good as its judges. Until a person's labels
//   (rr/eval/labels.hpp) show that the panel reads as they do, its scores
//   are reported, and the gate ignores them (rr/eval/gate.hpp). This
//   measures that agreement, judge by judge, for each kind of reading the
//   panel gives (facts told, sentences supported, which of two versions
//   reads better), against ADR-0019's bars:
//     1. Cohen's κ ≥ 0.4 (Cohen, 1960): the agreement beyond what chance
//        gives two readers who say each class as often as these two do.
//        Raw agreement flatters a judge on a skewed set: one that calls
//        every sentence supported agrees 95% of the time with a person
//        who finds one sentence in twenty unsupported, and has κ 0.
//     2. Per class, precision and recall each above a random judge's, by
//        the lower bound of its 95% interval. A judge that says a class at
//        its own rate q, whatever the truth, has a precision of the class's
//        share p among the person's verdicts, and a recall of q. So a
//        class clears its bars when precision's lower bound is above p and
//        recall's above q: the judge finds the class better than a coin
//        weighted as it is. The intervals are Wilson's (1927): on the few
//        items a class gets in an hour of labelling, the normal
//        approximation's interval is too narrow, and crosses 0 and 1.
//   A judge passes a kind when its κ clears 0.4 and every class either of
//   them used clears its bars; the panel passes when every judge passes
//   every kind (rr/eval/labels.hpp says why every kind).
//
//   For the parts read in both orders it also reports each judge's
//   position bias, how often its two orders disagreed, and its length
//   bias, how often it preferred the longer of two versions that differ by
//   a fifth or more, beside a half. ADR-0019 sets no bar on them: a person
//   reads them beside the agreement.
//
// Thread-safety: pure functions.
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rr/eval/stats.hpp>

namespace rr::eval {

inline constexpr double kKappaBar = 0.4;     // ADR-0019
inline constexpr double kKappaSlack = 1e-9;  // what a κ at the bar may fall under it by rounding

// `successes` of `trials`, with Wilson's score interval at 95%. None for no
// trial.
[[nodiscard]] std::optional<Interval> wilson(std::size_t successes, std::size_t trials);

// One class of a kind, as a person and a judge used it.
struct ClassAgreement {
    std::string name{};
    std::size_t person = 0;               // items the person put in it
    std::size_t judge = 0;                // items the judge put in it
    std::size_t both = 0;                 // items both did
    std::optional<Interval> precision{};  // both of the judge's; none when the judge never said it
    std::optional<Interval> recall{};     // both of the person's; none when the person never did
    double precision_bar = 0;             // the person's share: a random judge's precision
    double recall_bar = 0;                // the judge's share: a random judge's recall

    // Both lower bounds above their bars. A class one of the two never used
    // cannot clear them: the other said it, and was wrong each time.
    [[nodiscard]] bool clears() const noexcept;

    friend bool operator==(const ClassAgreement&, const ClassAgreement&) = default;
};

// What one judge and a person said of the same items, of one kind.
struct JudgeAgreement {
    std::string judge{};                    // its role
    std::size_t items = 0;                  // labelled items it read
    std::size_t unread = 0;                 // labelled items it left unread: left out
    std::optional<double> observed{};       // the share of items they agree on
    std::optional<double> chance{};         // the share two readers saying each class as often would agree on
    std::optional<double> kappa{};          // none for no item, or for one class only (chance is 1)
    std::vector<ClassAgreement> classes{};  // each class either used, in the kind's order

    // κ at least kKappaBar. A κ computed at the bar may fall a hair under it
    // (0.4 exactly is 2/9 over 5/9, 0.39999999999999997): kKappaSlack keeps
    // it at the bar.
    [[nodiscard]] bool kappa_clears() const noexcept;

    // κ clearing its bar, and every class clearing its bars.
    [[nodiscard]] bool passes() const noexcept;

    friend bool operator==(const JudgeAgreement&, const JudgeAgreement&) = default;
};

// An item's two verdicts, as indices into a kind's classes.
struct Verdicts {
    std::size_t person = 0;
    std::size_t judge = 0;
};

// The agreement of `judge` with the person over `verdicts`, of a kind whose
// classes are `classes`, in order; `unread` labelled items it did not read.
// Every index is below classes.size().
[[nodiscard]] JudgeAgreement agree(std::string judge, std::span<const std::string_view> classes,
                                   std::span<const Verdicts> verdicts, std::size_t unread);

// One judge's reading of a part in both orders, for its biases.
struct OrderedReading {
    bool flipped = false;          // its two orders disagreed
    std::optional<bool> longer{};  // whether a consistent preference was for the longer version;
                                   // none for a tie, a flip, or versions within a fifth of each other
};

// A part's two versions differ in length enough to see a length bias: the
// shorter at most four fifths of the longer, in words.
[[nodiscard]] bool lengths_differ(std::size_t a_words, std::size_t b_words) noexcept;

struct Biases {
    std::size_t read = 0;     // parts read in both orders
    std::size_t flipped = 0;  // of them, those whose orders disagreed
    std::optional<Interval> flip_rate{};
    std::size_t unequal = 0;  // consistent preferences between versions a fifth apart
    std::size_t longer = 0;   // of them, for the longer version
    std::optional<Interval> longer_rate{};

    friend bool operator==(const Biases&, const Biases&) = default;
};

[[nodiscard]] Biases biases(std::span<const OrderedReading> readings);

}  // namespace rr::eval

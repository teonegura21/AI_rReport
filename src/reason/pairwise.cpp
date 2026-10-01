#include <rr/reason/pairwise.hpp>

namespace rr::reason {

namespace {

// Which side one order preferred: `first` is a when a was shown first.
Winner as_shown(Preference preference, bool a_first) noexcept {
    switch (preference) {
        case Preference::first:
            return a_first ? Winner::a : Winner::b;
        case Preference::second:
            return a_first ? Winner::b : Winner::a;
        case Preference::tie:
            return Winner::tie;
    }
    return Winner::tie;
}

}  // namespace

std::string_view to_string(Winner winner) noexcept {
    switch (winner) {
        case Winner::a:
            return "a";
        case Winner::b:
            return "b";
        case Winner::tie:
            return "tie";
    }
    return "tie";
}

Winner swapped(Preference shown_a_first, Preference shown_b_first) noexcept {
    const Winner one = as_shown(shown_a_first, true);
    const Winner other = as_shown(shown_b_first, false);
    return one == other ? one : Winner::tie;
}

void PairwiseTally::add(Preference shown_a_first, Preference shown_b_first) noexcept {
    if (as_shown(shown_a_first, true) != as_shown(shown_b_first, false)) {
        ++inconsistent;
    }
    switch (swapped(shown_a_first, shown_b_first)) {
        case Winner::a:
            ++a;
            break;
        case Winner::b:
            ++b;
            break;
        case Winner::tie:
            ++ties;
            break;
    }
}

double PairwiseTally::score_a() const noexcept {
    if (pairs() == 0) {
        return 0.5;
    }
    return (static_cast<double>(a) + (0.5 * static_cast<double>(ties))) / static_cast<double>(pairs());
}

}  // namespace rr::reason

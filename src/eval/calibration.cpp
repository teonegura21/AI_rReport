// Whether the eval's judges read as a person does (rr/eval/calibration.hpp).
#include <rr/eval/calibration.hpp>

#include <algorithm>
#include <cmath>

#include <rr/core/assert.hpp>

namespace rr::eval {

std::optional<Interval> wilson(std::size_t successes, std::size_t trials) {
    RR_ASSERT(successes <= trials, "successes are some of the trials");
    if (trials == 0) {
        return std::nullopt;
    }
    const auto n = static_cast<double>(trials);
    const double p = static_cast<double>(successes) / n;
    const double z2 = kZ95 * kZ95;
    const double denominator = 1 + (z2 / n);
    const double centre = (p + (z2 / (2 * n))) / denominator;
    const double half = kZ95 * std::sqrt((p * (1 - p) / n) + (z2 / (4 * n * n))) / denominator;
    return Interval{.low = std::max(0.0, centre - half), .high = std::min(1.0, centre + half)};
}

bool ClassAgreement::clears() const noexcept {
    return precision.has_value() && recall.has_value() && precision->low > precision_bar &&
           recall->low > recall_bar;
}

bool JudgeAgreement::kappa_clears() const noexcept {
    return kappa.has_value() && *kappa + kKappaSlack >= kKappaBar;
}

bool JudgeAgreement::passes() const noexcept {
    return kappa_clears() &&
           std::ranges::all_of(classes, [](const ClassAgreement& one) { return one.clears(); });
}

JudgeAgreement agree(std::string judge, std::span<const std::string_view> classes,
                     std::span<const Verdicts> verdicts, std::size_t unread) {
    const std::size_t count = classes.size();
    // table[person][judge]: how many items the person put in one class and
    // the judge in the other.
    std::vector<std::vector<std::size_t>> table(count, std::vector<std::size_t>(count, 0));
    for (const Verdicts& one : verdicts) {
        RR_ASSERT(one.person < count, "a verdict is one of the kind's classes");
        RR_ASSERT(one.judge < count, "a verdict is one of the kind's classes");
        ++table[one.person][one.judge];
    }
    JudgeAgreement agreement{.judge = std::move(judge), .items = verdicts.size(), .unread = unread};
    if (verdicts.empty()) {
        return agreement;
    }
    const auto items = static_cast<double>(verdicts.size());
    double agreed = 0;
    double chance = 0;
    for (std::size_t index = 0; index < count; ++index) {
        ClassAgreement one{.name = std::string{classes[index]}};
        for (std::size_t other = 0; other < count; ++other) {
            one.person += table[index][other];
            one.judge += table[other][index];
        }
        one.both = table[index][index];
        agreed += static_cast<double>(one.both);
        const double person_share = static_cast<double>(one.person) / items;
        const double judge_share = static_cast<double>(one.judge) / items;
        chance += person_share * judge_share;
        if (one.person == 0 && one.judge == 0) {
            continue;  // neither used it: it says nothing
        }
        one.precision = wilson(one.both, one.judge);
        one.recall = wilson(one.both, one.person);
        one.precision_bar = person_share;
        one.recall_bar = judge_share;
        agreement.classes.push_back(std::move(one));
    }
    agreement.observed = agreed / items;
    agreement.chance = chance;
    // Chance agreement of 1 is one class only, both readers saying it of
    // every item: κ is 0 / 0, undefined, and says nothing of the judge.
    if (chance < 1) {
        agreement.kappa = (*agreement.observed - chance) / (1 - chance);
    }
    return agreement;
}

bool lengths_differ(std::size_t a_words, std::size_t b_words) noexcept {
    const std::size_t shorter = std::min(a_words, b_words);
    const std::size_t longer = std::max(a_words, b_words);
    return longer > 0 && shorter * 5 <= longer * 4;  // two empty versions are alike
}

Biases biases(std::span<const OrderedReading> readings) {
    Biases found;
    for (const OrderedReading& reading : readings) {
        ++found.read;
        if (reading.flipped) {
            ++found.flipped;
        }
        if (reading.longer.has_value()) {
            ++found.unequal;
            if (*reading.longer) {
                ++found.longer;
            }
        }
    }
    found.flip_rate = wilson(found.flipped, found.read);
    found.longer_rate = wilson(found.longer, found.unequal);
    return found;
}

}  // namespace rr::eval

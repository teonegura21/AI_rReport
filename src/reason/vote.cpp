#include <rr/reason/vote.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <set>
#include <utility>

#include <rr/core/assert.hpp>

namespace rr::reason {

namespace {

// Slack for floating point, so that a quorum of 0.6 settles three votes of five.
constexpr double kSlack = 1e-9;

bool reaches(int votes, int cast, double quorum) noexcept {
    return static_cast<double>(votes) + kSlack >= quorum * static_cast<double>(cast);
}

int leader(std::span<const Count> counts) noexcept {
    return counts.empty() ? 0 : counts.front().votes;
}

int runner_up(std::span<const Count> counts) noexcept {
    return counts.size() > 1 ? counts[1].votes : 0;
}

std::set<std::string> words(std::string_view text) {
    std::set<std::string> found;
    std::string word;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (std::isalnum(byte) != 0 || byte >= 0x80) {
            word += static_cast<char>(std::tolower(byte));
        } else if (!word.empty()) {
            found.insert(std::exchange(word, {}));
        }
    }
    if (!word.empty()) {
        found.insert(std::move(word));
    }
    return found;
}

}  // namespace

std::vector<std::string> waves_problems(const Waves& waves) {
    std::vector<std::string> problems;
    if (waves.first < 1) {
        problems.push_back(std::format("the first wave must draw a sample at least, not {}", waves.first));
    }
    if (waves.step < 0) {
        problems.push_back(std::format("a step cannot be negative ({})", waves.step));
    }
    if (waves.max < waves.first) {
        problems.push_back(std::format("max ({}) is below first ({})", waves.max, waves.first));
    } else if (waves.step == 0 && waves.max != waves.first) {
        problems.push_back(
            std::format("max ({}) is above first ({}), but with a step of 0 no wave draws the rest",
                        waves.max, waves.first));
    }
    if (std::isnan(waves.quorum) || waves.quorum <= 0.0 || waves.quorum > 1.0) {
        problems.push_back(std::format("a quorum is a share in (0, 1], not {}", waves.quorum));
    }
    if (std::isnan(waves.sure) || waves.sure < 0.0 || waves.sure > 1.0) {
        problems.push_back(
            std::format("how sure a lone sample must be is a probability in [0, 1], not {}", waves.sure));
    }
    return problems;
}

bool settled(std::span<const Count> counts, double quorum) noexcept {
    int cast = 0;
    for (const Count& count : counts) {
        cast += count.votes;
    }
    const int top = leader(counts);
    return top > runner_up(counts) && reaches(top, cast, quorum);
}

Result<std::vector<Consensus>> vote_keyed(std::span<const std::string> items, std::span<const Ballot> ballots,
                                          double quorum, double sure) {
    std::map<std::string_view, std::size_t> position;
    std::vector<Consensus> votes(items.size());
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (!position.emplace(items[index], index).second) {
            return fail(Errc::invalid_argument, std::format("'{}' is listed twice", items[index]));
        }
        votes[index].item = items[index];
    }

    std::vector<std::vector<const Ballot*>> cast(items.size());
    std::set<std::pair<std::size_t, int>> voted;  // (item, sample)
    for (const Ballot& ballot : ballots) {
        const auto found = position.find(ballot.item);
        if (found == position.end()) {
            return fail(Errc::invalid_argument,
                        std::format("a ballot on '{}', which is not voted on", ballot.item));
        }
        if (!voted.emplace(found->second, ballot.sample).second) {
            return fail(Errc::invalid_argument,
                        std::format("sample {} voted twice on '{}'", ballot.sample, ballot.item));
        }
        cast[found->second].push_back(&ballot);
    }

    for (std::size_t index = 0; index < items.size(); ++index) {
        Consensus& consensus = votes[index];
        for (const Ballot* ballot : cast[index]) {
            const auto same = std::ranges::find(consensus.counts, ballot->value, &Count::value);
            if (same == consensus.counts.end()) {
                consensus.counts.push_back(Count{.value = ballot->value, .votes = 1});
            } else {
                ++same->votes;
            }
        }
        // Stable: values with as many votes stay in the order first voted.
        std::ranges::stable_sort(consensus.counts, std::ranges::greater{}, &Count::votes);
        consensus.votes = static_cast<int>(cast[index].size());
        if (!settled(consensus.counts, quorum)) {
            continue;
        }
        // A lone sample settles its item only when it was sure of its answer.
        if (consensus.votes == 1 && cast[index].front()->confidence.value_or(0.0) + kSlack < sure) {
            continue;
        }
        consensus.winner = consensus.counts.front().value;
        std::vector<std::string> texts;
        std::vector<int> samples;
        for (const Ballot* ballot : cast[index]) {
            if (ballot->value == *consensus.winner) {
                texts.push_back(ballot->text);
                samples.push_back(ballot->sample);
            }
        }
        const std::optional<std::size_t> typical = medoid(texts);
        RR_ASSERT(typical.has_value(), "a settled item has the winner's ballots");
        consensus.medoid = samples[*typical];
    }
    return votes;
}

int next_wave(std::span<const Consensus> items, int drawn, const Waves& waves) noexcept {
    const int remaining = waves.max - drawn;
    if (remaining <= 0 || waves.step <= 0) {
        return 0;
    }
    const bool could_settle = std::ranges::any_of(items, [&](const Consensus& item) {
        if (item.winner.has_value()) {
            return false;
        }
        // At best, every sample still to come agrees with the leader.
        const int top = leader(item.counts) + remaining;
        return top > runner_up(item.counts) && reaches(top, item.votes + remaining, waves.quorum);
    });
    return could_settle ? std::min(waves.step, remaining) : 0;
}

double word_distance(std::string_view lhs, std::string_view rhs) {
    const std::set<std::string> left = words(lhs);
    const std::set<std::string> right = words(rhs);
    if (left.empty() && right.empty()) {
        return 0.0;
    }
    const auto shared = static_cast<std::size_t>(
        std::ranges::count_if(left, [&](const std::string& word) { return right.contains(word); }));
    const std::size_t either = left.size() + right.size() - shared;
    return 1.0 - (static_cast<double>(shared) / static_cast<double>(either));
}

std::optional<std::size_t> medoid(std::span<const std::string> texts, const Distance& distance) {
    if (texts.empty()) {
        return std::nullopt;
    }
    std::size_t best = 0;
    double least = std::numeric_limits<double>::infinity();
    for (std::size_t candidate = 0; candidate < texts.size(); ++candidate) {
        double sum = 0.0;
        for (std::size_t other = 0; other < texts.size(); ++other) {
            if (other != candidate) {
                sum += distance(texts[candidate], texts[other]);
            }
        }
        if (sum < least) {  // strictly: a tie keeps the first
            best = candidate;
            least = sum;
        }
    }
    return best;
}

}  // namespace rr::reason

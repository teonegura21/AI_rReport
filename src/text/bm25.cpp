#include <rr/text/bm25.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

#include <rr/core/assert.hpp>
#include <rr/text/tokens.hpp>

namespace rr::text {

Bm25::Bm25(Bm25Params params) : params_(params) {
    const bool in_range = params.k1 >= 0.0 && params.b >= 0.0 && params.b <= 1.0;  // false for NaN too
    RR_ASSERT(in_range, "BM25: k1 must be >= 0, and b within 0 to 1");
}

std::size_t Bm25::add(std::string_view passage) {
    const std::size_t index = lengths_.size();
    const std::vector<Term> found = terms(passage);
    std::unordered_map<std::string, std::size_t> counts;
    for (const Term& term : found) {
        ++counts[term.folded];
    }
    // Passages are added in order, so each posting list stays in order.
    for (const auto& [term, count] : counts) {
        postings_[term].push_back(Posting{.passage = index, .count = count});
    }
    lengths_.push_back(found.size());
    total_length_ += found.size();
    return index;
}

std::vector<Hit> Bm25::search(std::string_view query, std::size_t top) const {
    if (top == 0 || lengths_.empty() || total_length_ == 0) {
        return {};
    }
    // Each distinct term once, in the order the query has them: the sums are
    // made in one order, so equal inputs give equal scores to the last bit.
    std::vector<std::string> distinct;
    std::unordered_set<std::string> seen;
    for (const Term& term : terms(query)) {
        if (seen.insert(term.folded).second) {
            distinct.push_back(term.folded);
        }
    }

    const auto passages = static_cast<double>(lengths_.size());
    const double average_length = static_cast<double>(total_length_) / passages;
    std::vector<double> scores(lengths_.size(), 0.0);
    std::vector<bool> holds(lengths_.size(), false);
    for (const std::string& term : distinct) {
        const auto postings = postings_.find(term);
        if (postings == postings_.end()) {
            continue;
        }
        const auto holding = static_cast<double>(postings->second.size());
        const double idf = std::log(1.0 + ((passages - holding + 0.5) / (holding + 0.5)));
        for (const Posting& posting : postings->second) {
            const auto count = static_cast<double>(posting.count);
            const auto length = static_cast<double>(lengths_[posting.passage]);
            const double saturation = params_.k1 * (1.0 - params_.b + params_.b * length / average_length);
            scores[posting.passage] += idf * count * (params_.k1 + 1.0) / (count + saturation);
            holds[posting.passage] = true;
        }
    }

    std::vector<Hit> hits;
    for (std::size_t passage = 0; passage < scores.size(); ++passage) {
        if (holds[passage]) {
            hits.push_back(Hit{.passage = passage, .score = scores[passage]});
        }
    }
    const auto better = [](const Hit& lhs, const Hit& rhs) {
        return lhs.score != rhs.score ? lhs.score > rhs.score : lhs.passage < rhs.passage;
    };
    const auto kept = static_cast<std::ptrdiff_t>(std::min(top, hits.size()));
    std::partial_sort(hits.begin(), hits.begin() + kept, hits.end(), better);
    hits.resize(static_cast<std::size_t>(kept));
    return hits;
}

}  // namespace rr::text

// rr/text/bm25.hpp — which passages a query is most about.
//
// Why this exists
//   A judge model is asked whether a fact is in the article. It should not
//   read the whole article for each fact: long contexts lose facts in the
//   middle (Liu et al., 2023), and each call costs time. So the passages most
//   likely to hold the fact are retrieved first, and only those are judged.
//   The eval's key-fact recall does this (plan P24), and so does the search
//   for where an atom's content went.
//
// How
//   Okapi BM25 (Robertson and Zaragoza, 2009) over folded terms
//   (rr/text/tokens.hpp). A passage scores, for each distinct query term t,
//       idf(t) * f * (k1 + 1) / (f + k1 * (1 - b + b * length / average length))
//   where f is how often t occurs in the passage, and
//       idf(t) = ln(1 + (N - n + 0.5) / (n + 0.5))
//   for N passages, n of which hold t. This idf is never negative (Lucene's
//   form), so a common word adds little instead of subtracting. Defaults:
//   k1 = 1.2, b = 0.75. No stop words are removed: idf already weighs them
//   down, and in a transcript a "not" matters.
//
// Thread-safety: add() must not run concurrently with anything; search() may
// run concurrently with itself.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rr::text {

struct Bm25Params {
    double k1 = 1.2;  // how fast a term's repetitions saturate: >= 0
    double b = 0.75;  // how much a passage's length counts: 0 to 1
};

struct Hit {
    std::size_t passage = 0;  // its index, in the order passages were added
    double score = 0.0;

    friend bool operator==(const Hit&, const Hit&) = default;
};

class Bm25 {
public:
    // The parameters must satisfy their comments.
    explicit Bm25(Bm25Params params = {});

    // Adds a passage and returns its index: 0, 1, 2, ...
    std::size_t add(std::string_view passage);

    [[nodiscard]] std::size_t size() const noexcept { return lengths_.size(); }

    // The `top` passages that score highest for `query`, best first; ties go
    // to the earlier passage. Passages that hold no query term are left out.
    [[nodiscard]] std::vector<Hit> search(std::string_view query, std::size_t top) const;

private:
    struct Posting {
        std::size_t passage = 0;
        std::size_t count = 0;  // how often the term occurs in it
    };

    Bm25Params params_;
    std::vector<std::size_t> lengths_;  // each passage's terms
    std::size_t total_length_ = 0;
    std::unordered_map<std::string, std::vector<Posting>> postings_;  // passages in order
};

}  // namespace rr::text

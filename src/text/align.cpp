#include <rr/text/align.hpp>

#include <cstddef>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rr/core/assert.hpp>

namespace rr::text {

namespace {

// The best path into one cell of the alignment matrix: its score, what it
// matched, and where it started.
struct Cell {
    int score = 0;  // 0: no path ends here
    std::size_t matched = 0;
    std::size_t edits = 0;
    std::size_t quote_first = 0;  // the first quote term on the path
    std::size_t text_first = 0;   // the first text term on the path
};

// Each term as a number: its index among the quote's distinct terms. Text
// terms the quote lacks are -1, and never match.
std::pair<std::vector<int>, std::vector<int>> term_ids(const std::vector<Term>& quote,
                                                       const std::vector<Term>& text) {
    std::unordered_map<std::string_view, int> ids;
    std::vector<int> quote_ids;
    quote_ids.reserve(quote.size());
    for (const Term& term : quote) {
        quote_ids.push_back(ids.try_emplace(term.folded, static_cast<int>(ids.size())).first->second);
    }
    std::vector<int> text_ids;
    text_ids.reserve(text.size());
    for (const Term& term : text) {
        const auto found = ids.find(term.folded);
        text_ids.push_back(found == ids.end() ? -1 : found->second);
    }
    return {std::move(quote_ids), std::move(text_ids)};
}

// The best path into the cell where quote term `row` meets text term
// `column` (both counted from 1), from the three cells before it.
Cell step(const Cell& diagonal, const Cell& up, const Cell& left, bool equal, std::size_t row,
          std::size_t column, const AlignmentScores& scores) {
    Cell cell;
    // Along the diagonal: the two terms aligned, equal or not. A path starts
    // only on a match: a mismatch cannot raise a score above 0.
    if (const int score = diagonal.score + (equal ? scores.match : scores.mismatch); score > 0) {
        cell = diagonal.score > 0 ? diagonal : Cell{.quote_first = row - 1, .text_first = column - 1};
        cell.score = score;
        cell.matched += equal ? 1 : 0;
        cell.edits += equal ? 0 : 1;
    }
    // Down: a quote term the text lacks. Across: a text term the quote lacks.
    // Ties keep the diagonal, which matches more.
    for (const Cell* gap_from : {&up, &left}) {
        if (gap_from->score + scores.gap > cell.score) {
            cell = *gap_from;
            cell.score += scores.gap;
            cell.edits += 1;
        }
    }
    return cell;
}

}  // namespace

double Alignment::identity() const noexcept {
    return quote_terms == 0 ? 0.0 : static_cast<double>(matched) / static_cast<double>(quote_terms);
}

bool Alignment::verbatim() const noexcept {
    return quote_terms > 0 && matched == quote_terms && edits == 0;
}

Alignment align(std::string_view quote, std::string_view text, AlignmentScores scores) {
    return align(terms(quote), terms(text), scores);
}

Alignment align(const std::vector<Term>& quote, const std::vector<Term>& text, AlignmentScores scores) {
    const bool scores_align = scores.match > 0 && scores.mismatch <= 0 && scores.gap < 0;
    RR_ASSERT(scores_align, "alignment scores: a match must gain, a mismatch must not, a gap must cost");
    Alignment best{.quote_terms = quote.size()};
    if (quote.empty() || text.empty()) {
        return best;
    }
    const auto [quote_ids, text_ids] = term_ids(quote, text);

    // Two rows of the matrix: quote terms down, text terms across.
    std::vector<Cell> previous(text.size() + 1);
    std::vector<Cell> current(text.size() + 1);
    Cell top;
    std::size_t top_quote_end = 0;  // the best path's ends: one past its last terms
    std::size_t top_text_end = 0;
    for (std::size_t row = 1; row <= quote.size(); ++row) {
        current[0] = Cell{};
        for (std::size_t column = 1; column <= text.size(); ++column) {
            const bool equal = quote_ids[row - 1] == text_ids[column - 1];
            const Cell cell =
                step(previous[column - 1], previous[column], current[column - 1], equal, row, column, scores);
            current[column] = cell;
            if (cell.score > top.score ||
                (cell.score > 0 && cell.score == top.score && column < top_text_end)) {
                top = cell;
                top_quote_end = row;
                top_text_end = column;
            }
        }
        std::swap(previous, current);
    }
    if (top.score == 0) {
        return best;
    }
    best.score = top.score;
    best.matched = top.matched;
    best.edits = top.edits;
    best.quote = Span{.begin = quote[top.quote_first].span.begin, .end = quote[top_quote_end - 1].span.end};
    best.text = Span{.begin = text[top.text_first].span.begin, .end = text[top_text_end - 1].span.end};
    return best;
}

}  // namespace rr::text

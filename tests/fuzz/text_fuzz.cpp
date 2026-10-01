// The rr_text fuzz target. Whatever bytes arrive (a caption file in another
// encoding, a model's broken output), rr_text keeps its invariants, and never
// crashes, hangs or reads out of bounds. For every input:
//   * repair() gives valid UTF-8 and leaves valid text as it is; nfc() and
//     fold() succeed exactly on valid text, and give valid text;
//   * tokens, sentences, numbers and names are non-empty, in order, apart,
//     within the text, and start and end on code point boundaries;
//   * tokens cover every code point that is not a space or a control, and so
//     do sentences: nothing is lost. Sentences are trimmed;
//   * every number is finite, and no ordinal is a percentage;
//   * a stretch of a text aligns with the text verbatim, scoring every match;
//   * BM25 finds a passage by its own words, and ranks hits best first.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <rr/text/align.hpp>
#include <rr/text/bm25.hpp>
#include <rr/text/entities.hpp>
#include <rr/text/numbers.hpp>
#include <rr/text/sentences.hpp>
#include <rr/text/tokens.hpp>
#include <rr/text/unicode.hpp>

namespace rr::text {
namespace {

void require(bool holds, const char* invariant) {
    if (!holds) {
        (void)std::fprintf(stderr, "broken invariant: %s\n", invariant);
        std::abort();
    }
}

bool is_blank(char32_t code_point) {
    const CharClass kind = classify(code_point);
    return kind == CharClass::space || kind == CharClass::other;
}

// The text's code points, and where each starts and ends.
class Layout {
public:
    explicit Layout(std::string_view text)
        : size_(text.size())
        , points_(code_points(text))
        , starts_(text.size() + 1, kNone)
        , ends_(text.size() + 1, kNone) {
        std::size_t expected = 0;
        for (std::size_t index = 0; index < points_.size(); ++index) {
            require(points_[index].begin == expected && points_[index].end > points_[index].begin,
                    "code points follow each other");
            starts_[points_[index].begin] = index;
            ends_[points_[index].end] = index;
            expected = points_[index].end;
        }
        require(expected == text.size(), "code points cover the text");
    }

    [[nodiscard]] const std::vector<CodePoint>& points() const { return points_; }

    // Non-empty, in order, apart, within the text, on code point boundaries.
    void require_spans(const std::vector<Span>& spans, const char* invariant) const {
        std::size_t previous_end = 0;
        for (const Span& span : spans) {
            require(span.begin < span.end && span.end <= size_ && span.begin >= previous_end, invariant);
            require(starts_[span.begin] != kNone && ends_[span.end] != kNone, invariant);
            previous_end = span.end;
        }
    }

    // Every code point that is not a space or a control is inside a span.
    void require_covered(const std::vector<Span>& spans, const char* invariant) const {
        std::size_t next = 0;
        for (const CodePoint& point : points_) {
            if (is_blank(point.value)) {
                continue;
            }
            while (next < spans.size() && spans[next].end <= point.begin) {
                ++next;
            }
            require(next < spans.size() && spans[next].begin <= point.begin && point.end <= spans[next].end,
                    invariant);
        }
    }

    // A span's first and last code points are not spaces or controls.
    void require_trimmed(const std::vector<Span>& spans, const char* invariant) const {
        for (const Span& span : spans) {
            require(
                !is_blank(points_[starts_[span.begin]].value) && !is_blank(points_[ends_[span.end]].value),
                invariant);
        }
    }

private:
    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

    std::size_t size_;
    std::vector<CodePoint> points_;
    std::vector<std::size_t> starts_;  // byte offset -> the code point starting there
    std::vector<std::size_t> ends_;    // byte offset -> the code point ending there
};

void check_unicode(std::string_view text) {
    const bool valid = valid_utf8(text);
    const std::string repaired = repair(text);
    require(valid_utf8(repaired), "repair() gives valid UTF-8");
    require(valid == (repaired == text), "repair() changes exactly the invalid texts");
    const Result<std::string> canonical = nfc(text);
    const Result<std::string> folded = fold(text);
    require(canonical.has_value() == valid && folded.has_value() == valid,
            "nfc() and fold() refuse only invalid text");
    if (valid) {
        require(valid_utf8(*canonical) && valid_utf8(*folded), "nfc() and fold() give valid UTF-8");
    }
}

void check_tokens(std::string_view text, const Layout& layout) {
    std::vector<Span> spans;
    for (const Token& token : tokenize(text)) {
        spans.push_back(token.span);
    }
    layout.require_spans(spans, "tokens: in order, apart, on code point boundaries");
    layout.require_covered(spans, "tokens cover every code point but spaces and controls");
    for (const Term& term : terms(text)) {
        require(!term.folded.empty() && valid_utf8(term.folded), "a term folds to valid, non-empty text");
    }
}

std::vector<Span> check_sentences(const Layout& layout, std::string_view text) {
    const std::vector<Span> found = sentences(text);
    layout.require_spans(found, "sentences: in order, apart, on code point boundaries");
    layout.require_covered(found, "sentences lose nothing");
    layout.require_trimmed(found, "sentences are trimmed");
    return found;
}

void check_numbers(std::string_view text, const Layout& layout) {
    std::vector<Span> spans;
    for (const NumberMention& mention : numbers(text)) {
        require(std::isfinite(mention.value), "numbers are finite");
        require(!(mention.percent && mention.ordinal), "no ordinal is a percentage");
        spans.push_back(mention.span);
    }
    layout.require_spans(spans, "numbers: in order, apart, on code point boundaries");
}

void check_names(std::string_view text, const Layout& layout) {
    layout.require_spans(names(text), "names: in order, apart, on code point boundaries");
}

void check_alignment(std::string_view text) {
    // Quadratic: kept small, so that fuzzing stays fast.
    constexpr std::size_t kTextTerms = 600;
    constexpr std::size_t kQuoteTerms = 120;
    std::vector<Term> all = terms(text);
    all.resize(std::min(all.size(), kTextTerms));
    const std::vector<Term> quote(
        all.begin(), all.begin() + static_cast<std::ptrdiff_t>(std::min(all.size() / 2, kQuoteTerms)));

    const Alignment found = align(quote, all);
    require(found.quote_terms == quote.size() && found.matched <= found.quote_terms,
            "an alignment counts its terms");
    require(found.identity() >= 0.0 && found.identity() <= 1.0, "identity is a share");
    require(found.text.begin <= found.text.end && found.text.end <= text.size(),
            "an alignment is within the text");
    if (!quote.empty()) {
        require(found.verbatim(), "a stretch of a text aligns with it verbatim");
        require(found.score == 2 * static_cast<int>(quote.size()), "a verbatim alignment scores every match");
    }

    const std::string_view head = text.substr(0, std::min<std::size_t>(text.size() / 2, 400));
    const std::string_view tail = text.substr(head.size(), 800);
    const Alignment loose = align(head, tail);
    require(loose.score >= 0 && loose.matched <= loose.quote_terms, "any alignment is bounded");
    require(loose.quote.end <= head.size() && loose.text.end <= tail.size(),
            "any alignment is within its texts");
}

void check_retrieval(std::string_view text, const std::vector<Span>& passages) {
    constexpr std::size_t kTop = 5;
    Bm25 index;
    for (const Span& passage : passages) {
        index.add(text_of(text, passage));
    }
    for (std::size_t query = 0; query < std::min<std::size_t>(passages.size(), 3); ++query) {
        const std::string_view words = text_of(text, passages[query]);
        const std::vector<Hit> hits = index.search(words, kTop);
        require(hits.size() <= kTop, "at most `top` hits");
        require(terms(words).empty() || !hits.empty(), "a passage is found by its own words");
        for (std::size_t rank = 0; rank < hits.size(); ++rank) {
            require(hits[rank].passage < index.size() && std::isfinite(hits[rank].score) &&
                        hits[rank].score > 0.0,
                    "a hit is a passage with a positive score");
            require(
                rank == 0 || hits[rank - 1].score > hits[rank].score ||
                    (hits[rank - 1].score == hits[rank].score && hits[rank - 1].passage < hits[rank].passage),
                "hits are ranked best first, ties in passage order");
        }
    }
}

}  // namespace
}  // namespace rr::text

// libFuzzer's entry point: its name is libFuzzer's.
extern "C" int LLVMFuzzerTestOneInput(  // NOLINT(readability-identifier-naming)
    const std::uint8_t* data, std::size_t size) {
    const std::string_view text{reinterpret_cast<const char*>(data), size};
    const rr::text::Layout layout{text};
    rr::text::check_unicode(text);
    rr::text::check_tokens(text, layout);
    const std::vector<rr::text::Span> found = rr::text::check_sentences(layout, text);
    rr::text::check_numbers(text, layout);
    rr::text::check_names(text, layout);
    rr::text::check_alignment(text);
    rr::text::check_retrieval(text, found);
    return 0;
}

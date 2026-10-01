// Ledger B: the article against the atoms (rr/fidelity/checks.hpp).
#include <rr/knowledge/article.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rr/fidelity/checks.hpp>
#include <rr/text/align.hpp>
#include <rr/text/tokens.hpp>
#include <rr/text/unicode.hpp>

#include "held.hpp"

namespace rr::fidelity {

namespace {

using knowledge::Atom;
using knowledge::AtomId;
using knowledge::AtomLedger;
using knowledge::Block;
using knowledge::BlockKind;
using knowledge::SentenceId;
using knowledge::Transcript;

// The article as the checks read it: parsed, with each block's text as a
// reader sees it, and the blocks that cite each atom. Code blocks cite
// nothing (rr/knowledge/article.hpp), so no check reads their text.
class ArticleView {
public:
    explicit ArticleView(std::string_view markdown)
        : markdown_(markdown)
        , parsed_(knowledge::parse_article(markdown)) {
        for (std::size_t index = 0; index < parsed_.blocks.size(); ++index) {
            const Block& block = parsed_.blocks[index];
            texts_.push_back(knowledge::without_markers(markdown_, parsed_, block.span));
            for (const knowledge::CitingSentence& sentence : block.sentences) {
                for (const knowledge::Marker& marker : sentence.markers) {
                    std::vector<std::size_t>& blocks = citing_[marker.atom];
                    if (blocks.empty() || blocks.back() != index) {
                        blocks.push_back(index);
                    }
                }
            }
        }
    }

    [[nodiscard]] std::string_view markdown() const { return markdown_; }
    [[nodiscard]] const knowledge::ParsedArticle& parsed() const { return parsed_; }
    [[nodiscard]] const std::string& text(std::size_t block) const { return texts_[block]; }
    [[nodiscard]] text::Span span(std::size_t block) const { return parsed_.blocks[block].span; }

    // The blocks that cite `atom`, in order; none when it is not cited.
    [[nodiscard]] const std::vector<std::size_t>& citing(AtomId atom) const {
        static const std::vector<std::size_t> none;
        const auto found = citing_.find(atom);
        return found == citing_.end() ? none : found->second;
    }

private:
    std::string_view markdown_;
    knowledge::ParsedArticle parsed_;
    std::vector<std::string> texts_;
    std::map<AtomId, std::vector<std::size_t>> citing_;
};

Finding finding(Check check, Severity severity, std::string message, std::vector<SentenceId> sentences,
                std::vector<AtomId> atoms, std::optional<text::Span> article) {
    return Finding{.check = check,
                   .severity = severity,
                   .message = std::move(message),
                   .sentences = std::move(sentences),
                   .atoms = std::move(atoms),
                   .article = article};
}

// Markers are well formed and name atoms of the ledger.
void check_markers(const ArticleView& article, const AtomLedger& ledger, Report& report) {
    for (const knowledge::BadMarker& bad : article.parsed().bad_markers) {
        ++report.coverage.markers;
        ++report.coverage.markers_invalid;
        report.findings.push_back(finding(
            Check::marker_valid, Severity::error,
            std::format("{} is no marker: {}", text::text_of(article.markdown(), bad.span), bad.reason), {},
            {}, bad.span));
    }
    for (const Block& block : article.parsed().blocks) {
        for (const knowledge::CitingSentence& sentence : block.sentences) {
            for (const knowledge::Marker& marker : sentence.markers) {
                ++report.coverage.markers;
                if (ledger.find(marker.atom) == nullptr) {
                    ++report.coverage.markers_invalid;
                    report.findings.push_back(
                        finding(Check::marker_known, Severity::error,
                                std::format("[{}] names no atom: the atoms are A1 to A{}", marker.atom,
                                            ledger.atoms.size()),
                                {}, {marker.atom}, marker.span));
                }
            }
        }
    }
}

// Whether any of the citing blocks satisfies `holds`.
template <typename Predicate>
bool any_block(const ArticleView& article, const std::vector<std::size_t>& blocks, Predicate holds) {
    return std::ranges::any_of(blocks, [&](std::size_t block) { return holds(article.text(block)); });
}

// The atom's numbers, names and terms reach a paragraph that cites it.
void check_kept(const ArticleView& article, const Atom& atom, const std::vector<std::size_t>& blocks,
                Report& report) {
    const text::Span where = article.span(blocks.front());
    for (const text::NumberMention& number : held::numbers(atom.text)) {
        ++report.coverage.numbers;
        if (any_block(article, blocks,
                      [&](std::string_view text) { return held::states(held::numbers(text), number); })) {
            ++report.coverage.numbers_kept;
            continue;
        }
        report.findings.push_back(
            finding(Check::number_kept, Severity::error,
                    std::format("the paragraph citing {} lacks \"{}\": write it, in digits "
                                "or words",
                                atom.id, text::text_of(atom.text, number.span)),
                    atom.sentences, {atom.id}, where));
    }
    for (const std::string& name : atom.names) {
        if (!any_block(article, blocks, [&](std::string_view text) { return held::names(text, name); })) {
            report.findings.push_back(
                finding(Check::name_kept, Severity::error,
                        std::format("the paragraph citing {} does not name \"{}\"", atom.id, name),
                        atom.sentences, {atom.id}, where));
        }
    }
    for (const std::string& term : atom.terms) {
        if (!any_block(article, blocks, [&](std::string_view text) { return held::uses(text, term); })) {
            report.findings.push_back(
                finding(Check::term_kept, Severity::error,
                        std::format("the paragraph citing {} does not use the term \"{}\"", atom.id, term),
                        atom.sentences, {atom.id}, where));
        }
    }
}

// A must-quote stands, verbatim, in a paragraph that cites it.
void check_must_quote(const ArticleView& article, const Atom& atom, const std::vector<std::size_t>& blocks,
                      Report& report) {
    if (!atom.quote.has_value()) {
        return;
    }
    ++report.coverage.must_quotes;
    if (blocks.empty()) {
        return;  // atom-told says so
    }
    text::Alignment closest;
    std::size_t closest_block = blocks.front();
    for (const std::size_t block : blocks) {
        const text::Alignment found = text::align(*atom.quote, article.text(block));
        if (found.verbatim()) {
            ++report.coverage.must_quotes_kept;
            return;
        }
        if (found.score > closest.score) {
            closest = found;
            closest_block = block;
        }
    }
    const std::optional<std::string> instead = held::closest(closest, article.text(closest_block));
    const std::string has =
        instead.has_value() ? std::format("\"{}\"", *instead) : std::string{"nothing like it"};
    report.findings.push_back(
        finding(Check::must_quote_kept, Severity::error,
                std::format("{} must be quoted verbatim, \"{}\", and the paragraph citing it "
                            "has {}",
                            atom.id, held::excerpt(*atom.quote), has),
                atom.sentences, {atom.id}, article.span(closest_block)));
}

// Every atom is cited, and keeps what it holds where it is cited.
void check_atoms(const ArticleView& article, const AtomLedger& ledger, Report& report) {
    report.coverage.atoms += ledger.atoms.size();
    for (const Atom& atom : ledger.atoms) {
        const std::vector<std::size_t>& blocks = article.citing(atom.id);
        check_must_quote(article, atom, blocks, report);
        if (blocks.empty()) {
            report.findings.push_back(
                finding(Check::atom_told, Severity::error,
                        std::format("{} is not retold: tell it, and cite it [{}]", atom.id, atom.id),
                        atom.sentences, {atom.id}, std::nullopt));
            continue;
        }
        ++report.coverage.atoms_told;
        check_kept(article, atom, blocks, report);
    }
}

// Whether `code_point` is part of a word: a letter, a mark or a digit.
bool in_a_word(char32_t code_point) noexcept {
    const text::CharClass kind = text::classify(code_point);
    return kind == text::CharClass::letter || kind == text::CharClass::mark || kind == text::CharClass::digit;
}

// The quotations of a block: what stands between double quotes, curly or
// straight, in the article's offsets. A quote mark in inline code is code. A
// straight quote opens a quotation only where a word cannot be going on: after
// a letter or a digit, as in 13", it is an inch mark.
std::vector<text::Span> quotations(std::string_view markdown, const Block& block) {
    enum class Open : std::uint8_t {
        none,
        curly,
        straight
    };
    const std::vector<text::Span> code = knowledge::inline_code(markdown, block.span);
    auto next_code = code.begin();
    std::vector<text::Span> found;
    Open open = Open::none;
    std::size_t start = 0;
    char32_t before = U' ';
    for (const text::CodePoint& point : text::code_points(text::text_of(markdown, block.span))) {
        const std::size_t at = block.span.begin + point.begin;
        const char32_t previous = std::exchange(before, point.value);
        while (next_code != code.end() && next_code->end <= at) {
            ++next_code;
        }
        if (next_code != code.end() && next_code->begin <= at) {
            continue;  // a quote mark in code is code
        }
        if (point.value == U'\u201C' && open == Open::none) {
            open = Open::curly;
            start = block.span.begin + point.end;
        } else if ((point.value == U'\u201D' && open == Open::curly) ||
                   (point.value == U'"' && open == Open::straight)) {
            found.push_back(text::Span{.begin = start, .end = at});
            open = Open::none;
        } else if (point.value == U'"' && open == Open::none && !in_a_word(previous)) {
            open = Open::straight;
            start = block.span.begin + point.end;
        }
    }
    return found;
}

// A quotation's parts, cut where an ellipsis marks words left out.
std::vector<std::string> parts_of(std::string_view quotation) {
    std::vector<std::string> parts;
    std::string part;
    std::size_t at = 0;
    while (at < quotation.size()) {
        std::size_t skip = 0;
        for (const std::string_view ellipsis : {"[\u2026]", "[...]", "\u2026", "..."}) {
            if (quotation.substr(at).starts_with(ellipsis)) {
                skip = ellipsis.size();
                break;
            }
        }
        if (skip > 0) {
            parts.push_back(std::move(part));
            part.clear();
            at += skip;
        } else {
            part.push_back(quotation[at]);
            ++at;
        }
    }
    parts.push_back(std::move(part));
    return parts;
}

// Every quotation of three words or more is the transcript's words, verbatim
// part by part. Shorter ones are names, titles and terms.
void check_quotations(const ArticleView& article, const Transcript& transcript, Report& report) {
    constexpr std::size_t kQuotedWords = 3;
    std::vector<SentenceId> all;
    all.reserve(transcript.sentences.size());
    for (const knowledge::Sentence& sentence : transcript.sentences) {
        all.push_back(sentence.id);
    }
    const std::string whole = held::said(transcript, all);
    const std::vector<text::Term> said = text::terms(whole);
    for (const Block& block : article.parsed().blocks) {
        if (block.kind == BlockKind::code) {
            continue;
        }
        for (const text::Span& quotation : quotations(article.markdown(), block)) {
            const std::string quoted =
                knowledge::without_markers(article.markdown(), article.parsed(), quotation);
            if (text::terms(quoted).size() < kQuotedWords) {
                continue;
            }
            for (const std::string& part : parts_of(quoted)) {
                const std::vector<text::Term> words = text::terms(part);
                if (words.empty()) {
                    continue;
                }
                const text::Alignment found = text::align(words, said);
                if (found.verbatim()) {
                    continue;
                }
                const std::optional<std::string> instead = held::closest(found, whole);
                const std::string has =
                    instead.has_value()
                        ? std::format("is not what was said: the transcript has \"{}\"", *instead)
                        : std::string{"is nowhere in the transcript"};
                report.findings.push_back(
                    finding(Check::quote_verbatim, Severity::error,
                            std::format("the quote \"{}\" {}", held::excerpt(part), has), {}, {}, quotation));
                break;  // one finding per quotation
            }
        }
    }
}

// The atoms first cited in one section, with where each was first said (an
// atom's sentences are in order), in the order cited.
struct Told {
    AtomId atom;
    SentenceId said;
    text::Span where;
};

// Of `told`, the ones out of the talk's order: outside a longest run of
// non-decreasing positions, which is the fewest to move.
std::vector<std::size_t> out_of_order(const std::vector<Told>& told) {
    std::vector<std::size_t> tails;  // index into `told` of the last of each run length
    std::vector<std::size_t> previous(told.size(), told.size());
    for (std::size_t index = 0; index < told.size(); ++index) {
        const auto later = std::ranges::upper_bound(tails, told[index].said, std::less{},
                                                    [&](std::size_t tail) { return told[tail].said; });
        if (later != tails.begin()) {
            previous[index] = *(later - 1);
        }
        if (later == tails.end()) {
            tails.push_back(index);
        } else {
            *later = index;
        }
    }
    std::vector<bool> kept(told.size(), false);
    for (std::size_t index = tails.empty() ? told.size() : tails.back(); index < told.size();
         index = previous[index]) {
        kept[index] = true;
    }
    std::vector<std::size_t> moved;
    for (std::size_t index = 0; index < told.size(); ++index) {
        if (!kept[index]) {
            moved.push_back(index);
        }
    }
    return moved;
}

// Within a section (the blocks between headings), atoms are first told in
// the order they were said.
void check_order(const ArticleView& article, const AtomLedger& ledger, Report& report) {
    std::vector<Told> section;
    std::set<AtomId> seen;
    const auto close_section = [&] {
        for (const std::size_t index : out_of_order(section)) {
            const Told& told = section[index];
            report.findings.push_back(
                finding(Check::order_kept, Severity::warning,
                        std::format("{} was said at {}, before what this section tells ahead "
                                    "of it: keep the order of the talk",
                                    told.atom, told.said),
                        {told.said}, {told.atom}, told.where));
        }
        section.clear();
        seen.clear();
    };
    for (const Block& block : article.parsed().blocks) {
        if (block.kind == BlockKind::heading) {
            close_section();
        }
        for (const knowledge::CitingSentence& sentence : block.sentences) {
            for (const knowledge::Marker& marker : sentence.markers) {
                const Atom* atom = ledger.find(marker.atom);
                if (atom == nullptr || atom->sentences.empty() || !seen.insert(marker.atom).second) {
                    continue;
                }
                section.push_back(
                    Told{.atom = marker.atom, .said = atom->sentences.front(), .where = marker.span});
            }
        }
    }
    close_section();
}

}  // namespace

Report check_article(const Transcript& transcript, const AtomLedger& ledger, std::string_view markdown) {
    const ArticleView article{markdown};
    Report report;
    check_markers(article, ledger, report);
    check_atoms(article, ledger, report);
    check_quotations(article, transcript, report);
    check_order(article, ledger, report);
    return report;
}

}  // namespace rr::fidelity

#include <rr/knowledge/article.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <rr/core/assert.hpp>
#include <rr/knowledge/artifacts.hpp>
#include <rr/text/sentences.hpp>
#include <rr/text/unicode.hpp>

namespace rr::knowledge {

namespace {

using text::Span;

constexpr std::size_t kNone = std::string_view::npos;

// What a bracket holds, read as a citation.
struct Citation {
    std::optional<AtomId> atom{};  // a marker's atom
    std::string reason{};          // or why it is a bad marker; empty when it is no citation at all
};

bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

bool is_space(char c) noexcept {
    return c == ' ' || c == '\t';
}

std::string_view trim_spaces(std::string_view text) noexcept {
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

// The atom numbers written in `text`, in order: "A12, a13-14" gives 12, 13, 14.
std::vector<std::string_view> numbers_in(std::string_view text) {
    std::vector<std::string_view> found;
    std::size_t at = 0;
    while (at < text.size()) {
        if (!is_digit(text[at])) {
            ++at;
            continue;
        }
        const std::size_t begin = at;
        while (at < text.size() && is_digit(text[at])) {
            ++at;
        }
        found.push_back(text.substr(begin, at - begin));
    }
    return found;
}

// "[A12]" for the digits "012" or "12": the canonical marker, when there is one.
std::string canonical(std::string_view digits) {
    while (digits.size() > 1 && digits.front() == '0') {
        digits.remove_prefix(1);
    }
    return "[A" + std::string{digits} + "]";
}

// What the bracket's content says (see the header comment for the table).
Citation read_citation(std::string_view content) {
    const std::string_view trimmed = trim_spaces(content);
    if (trimmed.empty()) {
        return {};
    }
    const char letter = trimmed.front();
    const bool atom_letter = letter == 'A' || letter == 'a';
    if (!atom_letter && letter != 'S' && letter != 's' && letter != 'V' && letter != 'v') {
        return {};
    }
    const std::string_view rest = trim_spaces(trimmed.substr(1));
    if (letter == 'A' && (rest.empty() || rest.front() == '#')) {  // "[a]" is a footnote: text
        return {.reason = "the atom's number is missing: cite it as [A12]"};
    }
    if (rest.empty() || !is_digit(rest.front())) {
        return {};  // "[Applause]", "[Sic]", "[Video]": text
    }
    std::size_t digits_end = 0;
    while (digits_end < rest.size() && is_digit(rest[digits_end])) {
        ++digits_end;
    }
    const std::string_view digits = rest.substr(0, digits_end);
    const std::string_view tail = trim_spaces(rest.substr(digits_end));

    if (!tail.empty()) {
        const std::vector<std::string_view> numbers = numbers_in(rest);
        if (numbers.size() < 2) {
            return {.reason = "a marker holds an atom's ID only: " + canonical(digits)};
        }
        const bool range = tail.front() == '-' || tail.starts_with("\u2013") || tail.starts_with("to");
        return {.reason = range ? "one atom per marker, each atom of a range too: " +
                                      canonical(numbers.front()) + "\u2026" + canonical(numbers.back())
                                : "one atom per marker: " + canonical(numbers[0]) + canonical(numbers[1])};
    }
    if (!atom_letter) {
        return {.reason = "the article cites atoms, not sentences or frames: [A#]"};
    }
    if (digits.find_first_not_of('0') == kNone) {
        return {.reason = "atoms are numbered from A1"};
    }
    constexpr std::size_t kMaxDigits = 9;
    if (digits.size() - (digits.find_first_not_of('0')) > kMaxDigits) {
        return {.reason = "no atom has that number"};
    }
    const std::string written = "A" + std::string{digits};
    if (content != written || digits.front() == '0') {  // "a41", " A41 ", "A 41", "A041"
        return {.reason = "write it " + canonical(digits)};
    }
    return {.atom = AtomId::parse(written)};
}

// One line of the article: [begin, end), without its line break.
struct Line {
    std::size_t begin = 0;
    std::size_t end = 0;
};

std::vector<Line> lines_of(std::string_view markdown) {
    std::vector<Line> lines;
    std::size_t begin = 0;
    while (true) {
        const std::size_t newline = markdown.find('\n', begin);
        std::size_t end = newline == kNone ? markdown.size() : newline;
        if (end > begin && markdown[end - 1] == '\r') {
            --end;
        }
        lines.push_back(Line{.begin = begin, .end = end});
        if (newline == kNone) {
            return lines;
        }
        begin = newline + 1;
    }
}

std::size_t leading_spaces(std::string_view line) noexcept {
    std::size_t count = 0;
    while (count < line.size() && is_space(line[count])) {
        ++count;
    }
    return count;
}

// Where a block's prefix ends ("# ", "- ", "1. ", "> "), or kNone.
std::size_t heading_content(std::string_view line) noexcept {
    constexpr std::size_t kMaxIndent = 3;
    constexpr std::size_t kMaxLevel = 6;
    const std::size_t indent = leading_spaces(line);
    if (indent > kMaxIndent) {
        return kNone;
    }
    std::size_t at = indent;
    while (at < line.size() && line[at] == '#') {
        ++at;
    }
    const std::size_t level = at - indent;
    if (level == 0 || level > kMaxLevel || (at < line.size() && !is_space(line[at]))) {
        return kNone;
    }
    return at + leading_spaces(line.substr(at));
}

std::size_t list_content(std::string_view line) noexcept {
    constexpr std::size_t kMaxNumberDigits = 9;
    std::size_t at = leading_spaces(line);
    if (at < line.size() && (line[at] == '-' || line[at] == '*' || line[at] == '+')) {
        ++at;
    } else {
        const std::size_t digits = at;
        while (at < line.size() && is_digit(line[at])) {
            ++at;
        }
        if (at == digits || at - digits > kMaxNumberDigits || at >= line.size() ||
            (line[at] != '.' && line[at] != ')')) {
            return kNone;
        }
        ++at;
    }
    if (at < line.size() && !is_space(line[at])) {
        return kNone;
    }
    return at + leading_spaces(line.substr(at));
}

std::size_t quote_content(std::string_view line) noexcept {
    constexpr std::size_t kMaxIndent = 3;
    const std::size_t indent = leading_spaces(line);
    if (indent > kMaxIndent || indent >= line.size() || line[indent] != '>') {
        return kNone;
    }
    return indent + 1 + (indent + 1 < line.size() && is_space(line[indent + 1]) ? 1 : 0);
}

// A fence's character and length ("```", "~~~~"), or none.
struct Fence {
    char mark = '`';
    std::size_t length = 0;
};

std::optional<Fence> fence_of(std::string_view line) noexcept {
    constexpr std::size_t kMaxIndent = 3;
    constexpr std::size_t kMinLength = 3;
    const std::size_t indent = leading_spaces(line);
    if (indent > kMaxIndent || indent >= line.size() || (line[indent] != '`' && line[indent] != '~')) {
        return std::nullopt;
    }
    const char mark = line[indent];
    std::size_t at = indent;
    while (at < line.size() && line[at] == mark) {
        ++at;
    }
    if (at - indent < kMinLength) {
        return std::nullopt;
    }
    return Fence{.mark = mark, .length = at - indent};
}

bool closes(const Fence& open, std::string_view line) noexcept {
    const std::optional<Fence> fence = fence_of(line);
    return fence.has_value() && fence->mark == open.mark && fence->length >= open.length &&
           trim_spaces(line).size() == fence->length;
}

bool is_blank(std::string_view line) noexcept {
    return leading_spaces(line) == line.size();
}

// A block while the lines are read: its kind, its lines, and where its prose starts.
struct Open {
    BlockKind kind = BlockKind::paragraph;
    std::size_t begin = 0;
    std::size_t content_begin = 0;
    std::size_t end = 0;
};

class Parser {
public:
    explicit Parser(std::string_view markdown) : markdown_(markdown) {}

    ParsedArticle parse() {
        std::optional<Open> open;
        std::optional<Fence> fence;
        for (const Line& line : lines_of(markdown_)) {
            const std::string_view view = markdown_.substr(line.begin, line.end - line.begin);
            if (fence.has_value()) {
                RR_ASSERT(open.has_value(), "a fence opens a code block");
                open->end = line.end;
                if (closes(*fence, view)) {
                    finish(open);
                    fence.reset();
                }
                continue;
            }
            if (is_blank(view)) {
                finish(open);
                continue;
            }
            if (const std::optional<Fence> opened = fence_of(view)) {
                finish(open);
                open = Open{
                    .kind = BlockKind::code, .begin = line.begin, .content_begin = line.end, .end = line.end};
                fence = opened;
                continue;
            }
            if (const std::size_t at = heading_content(view); at != kNone) {
                finish(open);
                open = Open{.kind = BlockKind::heading,
                            .begin = line.begin,
                            .content_begin = line.begin + at,
                            .end = line.end};
                finish(open);
                continue;
            }
            if (const std::size_t at = list_content(view); at != kNone) {
                finish(open);
                open = Open{.kind = BlockKind::list_item,
                            .begin = line.begin,
                            .content_begin = line.begin + at,
                            .end = line.end};
                continue;
            }
            if (const std::size_t at = quote_content(view); at != kNone) {
                if (!open.has_value() || open->kind != BlockKind::quote) {
                    finish(open);
                    open = Open{.kind = BlockKind::quote,
                                .begin = line.begin,
                                .content_begin = line.begin + at,
                                .end = line.end};
                }
                open->end = line.end;
                continue;
            }
            // A plain line continues a paragraph, a list item or a quote.
            if (!open.has_value()) {
                open = Open{.kind = BlockKind::paragraph, .begin = line.begin, .content_begin = line.begin};
            }
            open->end = line.end;
        }
        finish(open);  // a fence never closed runs to the end
        return std::move(parsed_);
    }

private:
    void finish(std::optional<Open>& open) {
        if (!open.has_value()) {
            return;
        }
        Block block{.kind = open->kind, .span = Span{.begin = open->begin, .end = open->end}};
        if (open->kind != BlockKind::code && open->content_begin < open->end) {
            block.sentences = sentences_of(Span{.begin = open->content_begin, .end = open->end});
        }
        parsed_.blocks.push_back(std::move(block));
        open.reset();
    }

    // The sentences of a block's prose, with their markers. A marker follows
    // what it cites: markers at the start of a sentence, before any of its
    // words, belong to the sentence before ("One. [A12] Two." cites A12 in
    // "One."), and a sentence of markers alone joins it.
    std::vector<CitingSentence> sentences_of(Span prose) {
        const std::vector<Marker> markers = markers_in(prose);
        std::vector<CitingSentence> found;
        bool pending = false;  // a first sentence of markers alone, waiting for the next
        Span pending_span{};
        for (Span span : text::sentences(text::text_of(markdown_, prose))) {
            span = Span{.begin = span.begin + prose.begin, .end = span.end + prose.begin};
            const std::size_t words = first_word(span, markers);
            if (words == span.end) {
                if (!found.empty()) {
                    found.back().span.end = span.end;
                } else if (!pending) {
                    pending = true;
                    pending_span = span;
                } else {
                    pending_span.end = span.end;
                }
                continue;
            }
            const std::size_t leading_end = last_marker_end(span.begin, words, markers);
            if (!found.empty() && leading_end != kNone) {
                found.back().span.end = leading_end;
                span.begin = leading_end;
                while (span.begin < words && is_space(markdown_[span.begin])) {
                    ++span.begin;
                }
            }
            if (pending) {
                span.begin = pending_span.begin;
                pending = false;
            }
            found.push_back(CitingSentence{.span = span});
        }
        if (pending) {
            found.push_back(CitingSentence{.span = pending_span});
        }
        for (const Marker& marker : markers) {
            const auto holder = std::ranges::find_if(found, [&](const CitingSentence& sentence) {
                return sentence.span.begin <= marker.span.begin && marker.span.end <= sentence.span.end;
            });
            if (holder != found.end()) {
                holder->markers.push_back(marker);
            }
        }
        return found;
    }

    // Where the first letter or digit of `span` outside its markers is, or
    // the span's end when it has none.
    [[nodiscard]] std::size_t first_word(Span span, const std::vector<Marker>& markers) const {
        std::size_t next = 0;
        for (const text::CodePoint& point : text::code_points(text::text_of(markdown_, span))) {
            const std::size_t at = span.begin + point.begin;
            while (next < markers.size() && markers[next].span.end <= at) {
                ++next;
            }
            if (next < markers.size() && markers[next].span.begin <= at) {
                continue;
            }
            const text::CharClass kind = text::classify(point.value);
            if (kind == text::CharClass::letter || kind == text::CharClass::digit) {
                return at;
            }
        }
        return span.end;
    }

    // The end of the last marker in [first, last), or kNone.
    static std::size_t last_marker_end(std::size_t first, std::size_t last,
                                       const std::vector<Marker>& markers) {
        std::size_t end = kNone;
        for (const Marker& marker : markers) {
            if (marker.span.begin >= first && marker.span.end <= last) {
                end = marker.span.end;
            }
        }
        return end;
    }

    // The markers in `prose`, outside inline code; bad markers are recorded.
    std::vector<Marker> markers_in(Span prose) {
        constexpr std::size_t kMaxContent = 64;
        const std::vector<Span> code = inline_code(markdown_, prose);
        std::vector<Marker> markers;
        std::size_t code_index = 0;
        for (std::size_t at = prose.begin; at < prose.end; ++at) {
            while (code_index < code.size() && code[code_index].end <= at) {
                ++code_index;
            }
            if (code_index < code.size() && code[code_index].begin <= at) {
                at = code[code_index].end - 1;
                continue;
            }
            if (markdown_[at] != '[' || (at > prose.begin && markdown_[at - 1] == '\\')) {
                continue;
            }
            std::size_t end = at + 1;
            while (end < prose.end && end - at <= kMaxContent && markdown_[end] != ']' &&
                   markdown_[end] != '[' && markdown_[end] != '\n') {
                ++end;
            }
            const bool closed = end < prose.end && markdown_[end] == ']';
            const Citation citation = read_citation(markdown_.substr(at + 1, end - at - 1));
            if (!citation.atom.has_value() && citation.reason.empty()) {
                continue;
            }
            if (!closed) {
                parsed_.bad_markers.push_back(BadMarker{.span = Span{.begin = at, .end = end},
                                                        .reason = "an unclosed marker: close it with ]"});
                at = end - 1;
                continue;
            }
            const Span span{.begin = at, .end = end + 1};
            if (citation.atom.has_value()) {
                markers.push_back(Marker{.atom = *citation.atom, .span = span});
            } else {
                parsed_.bad_markers.push_back(BadMarker{.span = span, .reason = citation.reason});
            }
            at = end;
        }
        return markers;
    }

    std::string_view markdown_;
    ParsedArticle parsed_;
};

}  // namespace

json::Json to_json(const Article& article) {
    return json::Json{{"markdown", article.markdown}};
}

Result<Article> article_from_json(const json::Json& document) {
    RR_TRY(artifact_schemas().validate("artifacts/article", document));
    return Article{.markdown = document.at("markdown").get<std::string>()};
}

std::vector<AtomId> ParsedArticle::cited() const {
    std::vector<AtomId> atoms;
    std::unordered_set<AtomId> seen;
    for (const Block& block : blocks) {
        for (const CitingSentence& sentence : block.sentences) {
            for (const Marker& marker : sentence.markers) {
                if (seen.insert(marker.atom).second) {
                    atoms.push_back(marker.atom);
                }
            }
        }
    }
    return atoms;
}

ParsedArticle parse_article(std::string_view markdown) {
    return Parser{markdown}.parse();
}

std::string without_markers(std::string_view markdown, const ParsedArticle& parsed, Span span) {
    std::string text;
    std::size_t at = span.begin;
    for (const Block& block : parsed.blocks) {
        for (const CitingSentence& sentence : block.sentences) {
            for (const Marker& marker : sentence.markers) {
                if (marker.span.begin < span.begin || marker.span.end > span.end) {
                    continue;
                }
                std::size_t kept_end = marker.span.begin;
                while (kept_end > at && is_space(markdown[kept_end - 1])) {
                    --kept_end;
                }
                text.append(markdown.substr(at, kept_end - at));
                at = marker.span.end;
            }
        }
    }
    text.append(markdown.substr(at, span.end - at));
    const std::size_t first = text.find_first_not_of(" \t");
    return first == std::string::npos ? std::string{} : text.substr(first);
}

std::vector<Span> inline_code(std::string_view markdown, Span span) {
    const bool within = span.begin <= span.end && span.end <= markdown.size();
    RR_ASSERT(within, "the span is of the markdown");
    const auto run_end = [&](std::size_t at) {
        while (at < span.end && markdown[at] == '`') {
            ++at;
        }
        return at;
    };
    std::vector<Span> spans;
    std::size_t at = span.begin;
    while (at < span.end) {
        if (markdown[at] != '`') {
            ++at;
            continue;
        }
        const std::size_t opening = run_end(at);
        std::optional<std::size_t> closing;
        for (std::size_t search = opening; search < span.end;) {
            if (markdown[search] != '`') {
                ++search;
                continue;
            }
            const std::size_t end = run_end(search);
            if (end - search == opening - at) {
                closing = end;
                break;
            }
            search = end;
        }
        if (closing.has_value()) {
            spans.push_back(Span{.begin = at, .end = *closing});
            at = *closing;
        } else {
            at = opening;  // unmatched backticks are text
        }
    }
    return spans;
}

}  // namespace rr::knowledge

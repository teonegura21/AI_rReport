// What every caption format shares (cues.hpp).
#include "cues.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rr/core/assert.hpp>
#include <rr/core/result.hpp>
#include <rr/text/unicode.hpp>

namespace rr::transcript::cues {

namespace {

constexpr std::size_t kNone = std::string_view::npos;

// A character reference's name, at most: "&#x10FFFF;" is the longest kept.
constexpr std::size_t kMaxReference = 10;

struct Named {
    std::string_view name;
    std::string_view text;
};

// The references captions use. "&lrm;" and "&rlm;" mark direction, which
// plain text does not keep: they decode to nothing.
constexpr auto kNamed = std::to_array<Named>({
    {.name = "amp", .text = "&"},
    {.name = "lt", .text = "<"},
    {.name = "gt", .text = ">"},
    {.name = "quot", .text = "\""},
    {.name = "apos", .text = "'"},
    {.name = "nbsp", .text = "\u00A0"},
    {.name = "lrm", .text = ""},
    {.name = "rlm", .text = ""},
    {.name = "hellip", .text = "\u2026"},
    {.name = "mdash", .text = "\u2014"},
    {.name = "ndash", .text = "\u2013"},
    {.name = "lsquo", .text = "\u2018"},
    {.name = "rsquo", .text = "\u2019"},
    {.name = "ldquo", .text = "\u201C"},
    {.name = "rdquo", .text = "\u201D"},
});

// The value of a numeric reference's digits ("39", "x27"), or nullopt.
std::optional<char32_t> value_of(std::string_view digits) {
    constexpr char32_t kLast = 0x10FFFF;
    constexpr unsigned kDecimal = 10;
    constexpr unsigned kHex = 16;
    const bool hex = digits.starts_with('x') || digits.starts_with('X');
    const unsigned base = hex ? kHex : kDecimal;
    if (hex) {
        digits.remove_prefix(1);
    }
    if (digits.empty()) {
        return std::nullopt;
    }
    char32_t value = 0;
    for (const char digit : digits) {
        unsigned of_digit = 0;
        if (digit >= '0' && digit <= '9') {
            of_digit = static_cast<unsigned>(digit - '0');
        } else if (hex && digit >= 'a' && digit <= 'f') {
            of_digit = static_cast<unsigned>(digit - 'a') + kDecimal;
        } else if (hex && digit >= 'A' && digit <= 'F') {
            of_digit = static_cast<unsigned>(digit - 'A') + kDecimal;
        } else {
            return std::nullopt;
        }
        value = (value * base) + of_digit;
        if (value > kLast) {
            return std::nullopt;
        }
    }
    return value == 0 ? std::nullopt : std::optional<char32_t>{value};
}

// The text of the reference `&name;`, or nullopt when rReport does not know it.
std::optional<std::string> reference(std::string_view name) {
    if (name.starts_with('#')) {
        const std::optional<char32_t> value = value_of(name.substr(1));
        if (!value.has_value()) {
            return std::nullopt;
        }
        std::string text;
        text::append_utf8(text, *value);
        return text;
    }
    const auto* const named = std::ranges::find(kNamed, name, &Named::name);
    if (named == kNamed.end()) {
        return std::nullopt;
    }
    return std::string{named->text};
}

// `written` as a transcript keeps text: valid UTF-8 (rr/text/unicode.hpp), NFC.
std::string kept(std::string_view written) {
    Result<std::string> normalized = text::nfc(text::repair(written));
    RR_ASSERT(normalized.has_value(), "repaired text is valid UTF-8");
    return std::move(*normalized);
}

// A space, or what rr/text never reads as text: a control, a format mark, an
// unassigned code point. Words are what lies between (and a NUL byte in a
// caption file is no part of one).
bool is_blank(char32_t code_point) noexcept {
    const text::CharClass kind = text::classify(code_point);
    return kind == text::CharClass::space || kind == text::CharClass::other;
}

// The zero-width non-joiner and joiner, which shape the characters around
// them (Persian words, emoji): between two characters of a word, they are
// part of it.
bool is_joiner(char32_t code_point) noexcept {
    return code_point == U'\u200C' || code_point == U'\u200D';
}

// The notes that set lyrics apart: "♪ la la ♪". The lyrics are words; the
// notes are not.
bool is_note(char32_t code_point) noexcept {
    return code_point == U'\u2669' || code_point == U'\u266A' || code_point == U'\u266B' ||
           code_point == U'\u266C';
}

// Whether an annotation's text says a word was not heard: "[inaudible]".
bool says_unclear(std::string_view annotation) {
    std::string lower{annotation};
    std::ranges::transform(lower, lower.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return std::ranges::any_of(
        std::to_array<std::string_view>({"inaudible", "unintelligible", "indistinct", "unclear"}),
        [&](std::string_view said) { return lower.contains(said); });
}

// When each point of a cue's text was said: the cue's start at its first
// byte, its end after its last, and the format's marks between, each moved
// past the spaces before the word it times.
class Clock {
public:
    explicit Clock(const Cue& cue) {
        const Millis end = std::max(cue.end, cue.start);
        anchors_.push_back(Mark{.offset = 0, .time = cue.start});
        for (const Mark& mark : cue.marks) {
            std::size_t offset = mark.offset;
            while (offset < cue.text.size() && (cue.text[offset] == ' ' || cue.text[offset] == '\n')) {
                ++offset;
            }
            if (offset < anchors_.back().offset || offset > cue.text.size()) {
                continue;  // out of order: a format's glitch
            }
            anchors_.push_back(
                Mark{.offset = offset, .time = std::clamp(mark.time, anchors_.back().time, end)});
        }
        anchors_.push_back(Mark{.offset = cue.text.size(), .time = std::max(end, anchors_.back().time)});
    }

    // The time at byte `offset` of the cue's text, between the anchors around it.
    [[nodiscard]] Millis at(std::size_t offset) const {
        const auto after = std::ranges::upper_bound(anchors_, offset, {}, &Mark::offset);
        if (after == anchors_.end()) {
            return anchors_.back().time;
        }
        const Mark& before = *(after - 1);  // anchors_ starts at offset 0, so `after` is never the first
        const auto span = static_cast<std::int64_t>(after->offset - before.offset);
        const auto into = static_cast<std::int64_t>(offset - before.offset);
        return before.time + ((after->time - before.time) * into / span);
    }

private:
    std::vector<Mark> anchors_;
};

enum class PieceKind : std::uint8_t {
    word,
    sound,
};

// A word or a sound of a cue, where it is in the cue's text.
struct Piece {
    PieceKind kind = PieceKind::word;
    std::size_t begin = 0;
    std::size_t end = 0;
    std::optional<std::string> text{};     // when not the text at [begin, end): "[unclear]"
    std::optional<std::string> speaker{};  // a captioner's label
    bool turn = false;                     // a new speaker starts with it
};

// The captioner's label that names the speaker after it, as read.
struct Label {
    std::string name;
    std::size_t next = 0;  // the point after its colon
};

// Reads a cue's text, line by line, into pieces. The speaker a label names
// lasts until the next turn, across cues: it is the reader's state.
class Reader {
public:
    std::vector<Piece> read(std::string_view text) {
        text_ = text;
        points_ = text::code_points(text);
        closers_.assign(points_.size(), {kNone, kNone});
        std::array<std::size_t, 2> next{kNone, kNone};
        for (std::size_t index = points_.size(); index-- > 0;) {
            const char32_t value = points_[index].value;
            if (value == U'\n') {
                next = {kNone, kNone};
            } else if (value == U']') {
                next[0] = index;
            } else if (value == U')') {
                next[1] = index;
            }
            closers_[index] = next;
        }
        pieces_.clear();
        std::size_t line = 0;
        for (std::size_t index = 0; index <= points_.size(); ++index) {
            if (index == points_.size() || points_[index].value == U'\n') {
                read_line(line, index);
                line = index + 1;
            }
        }
        return std::move(pieces_);
    }

private:
    [[nodiscard]] char32_t at(std::size_t index) const { return points_[index].value; }

    [[nodiscard]] std::size_t after_spaces(std::size_t index, std::size_t last) const {
        while (index < last && is_blank(at(index))) {
            ++index;
        }
        return index;
    }

    // After a turn marker at `index`: ">>" (or more), or a dialogue dash and a
    // space ("- Hi."); `index` when there is none.
    [[nodiscard]] std::size_t after_turn(std::size_t index, std::size_t last) const {
        std::size_t marks = index;
        while (marks < last && at(marks) == U'>') {
            ++marks;
        }
        if (marks - index >= 2) {
            return after_spaces(marks, last);
        }
        const bool dash = index < last && (at(index) == U'-' || at(index) == U'\u2013');
        if (dash && index + 1 < last && is_blank(at(index + 1))) {
            return after_spaces(index + 1, last);
        }
        return index;
    }

    // A captioner's label at `index`: one to three words in capitals, a colon,
    // and a space or the line's end: "ADA LOVELACE: ...".
    [[nodiscard]] std::optional<Label> label_at(std::size_t index, std::size_t last) const {
        constexpr std::size_t kMaxWords = 3;
        constexpr std::size_t kMinLetters = 2;
        std::size_t words = 0;
        std::size_t letters = 0;
        std::size_t point = index;
        while (point < last && words < kMaxWords) {
            if (!text::is_upper(at(point))) {
                return std::nullopt;  // a word of a label starts with a capital
            }
            while (point < last && (text::is_upper(at(point)) || inside_label_word(at(point)))) {
                letters += text::is_upper(at(point)) ? 1U : 0U;
                ++point;
            }
            ++words;
            if (point < last && at(point) == U':') {
                const bool ends = point + 1 == last || is_blank(at(point + 1));
                if (!ends || letters < kMinLetters) {
                    return std::nullopt;
                }
                return Label{.name = kept(text_.substr(points_[index].begin,
                                                       points_[point].begin - points_[index].begin)),
                             .next = after_spaces(point + 1, last)};
            }
            if (point >= last || at(point) != U' ') {
                return std::nullopt;
            }
            ++point;
        }
        return std::nullopt;
    }

    [[nodiscard]] static bool inside_label_word(char32_t value) noexcept {
        return value == U'.' || value == U'\'' || value == U'\u2019' || value == U'-' ||
               text::classify(value) == text::CharClass::mark;
    }

    void read_line(std::size_t first, std::size_t last) {
        std::size_t index = after_spaces(first, last);
        if (const std::size_t next = after_turn(index, last); next != index) {
            turn_ = true;
            labelled_.reset();  // a new speaker, not named
            index = next;
        }
        if (std::optional<Label> label = label_at(index, last)) {
            turn_ = true;
            labelled_ = std::move(label->name);
            index = label->next;
        }
        while (index < last) {
            const char32_t value = at(index);
            if (is_blank(value) || is_note(value)) {
                ++index;
            } else if (const std::size_t close = closer(index); close != kNone) {
                annotation(index, close);
                index = close + 1;
            } else {
                index = word(index, last);
            }
        }
    }

    // The closing bracket of an annotation opening at `index`, on its line.
    [[nodiscard]] std::size_t closer(std::size_t index) const {
        if (at(index) == U'[') {
            return closers_[index][0];
        }
        return at(index) == U'(' ? closers_[index][1] : kNone;
    }

    void annotation(std::size_t open, std::size_t close) {
        const std::size_t begin = points_[open].begin;
        const std::size_t end = points_[close].end;
        if (says_unclear(text_.substr(begin, end - begin))) {
            add(Piece{.kind = PieceKind::word, .begin = begin, .end = end, .text = "[unclear]"});
        } else {
            pieces_.push_back(Piece{.kind = PieceKind::sound, .begin = begin, .end = end});
        }
    }

    // Reads the word at `index`, up to a blank, a note or an annotation; the
    // point after it.
    std::size_t word(std::size_t index, std::size_t last) {
        std::size_t end = index + 1;
        while (end < last && !is_note(at(end)) && closer(end) == kNone) {
            const bool joins =
                is_joiner(at(end)) && end + 1 < last && !is_blank(at(end + 1)) && !is_note(at(end + 1));
            if (is_blank(at(end)) && !joins) {
                break;
            }
            ++end;
        }
        add(Piece{.kind = PieceKind::word, .begin = points_[index].begin, .end = points_[end - 1].end});
        return end;
    }

    void add(Piece piece) {
        piece.speaker = labelled_;
        piece.turn = std::exchange(turn_, false);
        pieces_.push_back(std::move(piece));
    }

    std::optional<std::string> labelled_;  // the speaker a label named, until the next turn
    std::string_view text_;
    std::vector<text::CodePoint> points_;
    std::vector<std::array<std::size_t, 2>> closers_;  // the next ']' and ')' on the line, by point
    std::vector<Piece> pieces_;
    bool turn_ = false;  // a turn waits for its first word
};

// The voice that speaks byte `offset` of a cue, if any.
std::optional<std::string> voice_at(const Cue& cue, std::size_t offset) {
    for (const Voice& voice : cue.voices) {
        if (offset >= voice.begin && offset < voice.end) {
            return voice.name;
        }
    }
    return std::nullopt;
}

// Starts never decrease, ends never precede starts, and an end runs no later
// than the next start.
template <typename Timed>
void keep_in_order(std::vector<Timed>& timed) {
    for (std::size_t index = 0; index < timed.size(); ++index) {
        if (index > 0) {
            timed[index].start = std::max(timed[index].start, timed[index - 1].start);
        }
        timed[index].end = std::max(timed[index].end, timed[index].start);
    }
    for (std::size_t index = 0; index + 1 < timed.size(); ++index) {
        timed[index].end = std::min(timed[index].end, timed[index + 1].start);
    }
}

}  // namespace

std::string decode_references(std::string_view text) {
    std::string decoded;
    decoded.reserve(text.size());
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t semicolon = text[at] == '&' ? text.find(';', at + 1) : kNone;
        if (semicolon != kNone && semicolon - at - 1 <= kMaxReference) {
            if (const std::optional<std::string> known = reference(text.substr(at + 1, semicolon - at - 1))) {
                decoded += *known;
                at = semicolon + 1;
                continue;
            }
        }
        decoded.push_back(text[at]);
        ++at;
    }
    return decoded;
}

void add_cues(std::vector<Cue> cues, CaptionTrack& track) {
    std::ranges::stable_sort(cues, {}, &Cue::start);
    Reader reader;
    for (const Cue& cue : cues) {
        const Clock clock{cue};
        bool first = true;
        for (Piece& piece : reader.read(cue.text)) {
            const Millis start = clock.at(piece.begin);
            const Millis end = clock.at(piece.end);
            std::string written = piece.text.has_value()
                                      ? *std::move(piece.text)
                                      : kept(cue.text.substr(piece.begin, piece.end - piece.begin));
            if (piece.kind == PieceKind::sound) {
                track.sounds.push_back(Sound{.text = std::move(written), .start = start, .end = end});
                continue;
            }
            std::optional<std::string> speaker = voice_at(cue, piece.begin);
            if (!speaker.has_value()) {
                speaker = std::move(piece.speaker);
            }
            const bool changed = !track.words.empty() && track.words.back().speaker != speaker;
            const bool paragraph = piece.turn || (first && cue.paragraph) || changed;
            track.words.push_back(Word{.text = std::move(written),
                                       .start = start,
                                       .end = end,
                                       .speaker = std::move(speaker),
                                       .paragraph = paragraph});
            first = false;
        }
    }
    keep_in_order(track.words);
    keep_in_order(track.sounds);
}

}  // namespace rr::transcript::cues
